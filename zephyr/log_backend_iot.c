/*
 * ESP IoT Log - Zephyr Logging Backend
 *
 * Hooks into Zephyr's LOG subsystem so all LOG_INF/LOG_ERR/etc. calls
 * are automatically forwarded over IPv6 multicast when a listener is
 * active. No changes needed to application log statements.
 *
 * IMPORTANT: The log backend process() runs in the logging thread which
 * has a small stack. We CANNOT call zsock_sendto() from here — it would
 * overflow the stack. Instead, we queue formatted messages into a ring
 * buffer and drain them from iot_log_poll() in the main thread.
 */

#include "iot_log_zephyr.h"

#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include <string.h>

/* Ring buffer for queued log messages.
 * Each entry: 1 byte level + 2 bytes length + message text.
 * 2KB holds ~10-20 typical log messages. */
#define IOT_LOG_RING_SIZE 2048
static uint8_t ring_data[IOT_LOG_RING_SIZE];
static struct ring_buf ring;
static bool ring_inited;

/* Output buffer for log formatting (logging thread is single-threaded). */
static uint8_t log_buf[512];

/* Accumulation buffer for the current message. */
static char msg_buf[500];
static size_t msg_pos;

static int char_out(uint8_t *data, size_t length, void *ctx)
{
    ARG_UNUSED(ctx);

    for (size_t i = 0; i < length && msg_pos < sizeof(msg_buf) - 1; i++) {
        if (data[i] != '\n' && data[i] != '\r') {
            msg_buf[msg_pos++] = data[i];
        }
    }

    return length;
}

LOG_OUTPUT_DEFINE(log_output_iot, char_out, log_buf, sizeof(log_buf));

static void process(const struct log_backend *const backend,
                    union log_msg_generic *msg)
{
    if (!iot_log_is_active() || !ring_inited) {
        return;
    }

    /* Format the message — skip level and timestamp since the binary
     * protocol header carries both. Keep module name for context. */
    msg_pos = 0;
    uint32_t flags = 0;
    log_format_func_t log_output_func = log_format_func_t_get(LOG_OUTPUT_TEXT);
    if (log_output_func == NULL) {
        /* Text formatter not available (CONFIG_LOG_OUTPUT_FORMAT_TEXT disabled) */
        return;
    }
    log_output_func(&log_output_iot, &msg->log, flags);

    if (msg_pos == 0) {
        return;
    }

    /* Map Zephyr log level to iot_log level */
    uint8_t zephyr_level = log_msg_get_level(&msg->log);
    uint8_t level;
    switch (zephyr_level) {
    case LOG_LEVEL_ERR:  level = IOT_LOG_ERROR;   break;
    case LOG_LEVEL_WRN:  level = IOT_LOG_WARN;    break;
    case LOG_LEVEL_INF:  level = IOT_LOG_INFO;    break;
    case LOG_LEVEL_DBG:  level = IOT_LOG_DEBUG;   break;
    default:             level = IOT_LOG_VERBOSE;  break;
    }

    /* Queue: [level:1][len_hi:1][len_lo:1][msg:len] */
    uint16_t len = (uint16_t)msg_pos;
    uint8_t header[3] = { level, (uint8_t)(len >> 8), (uint8_t)(len & 0xFF) };

    uint32_t needed = 3 + len;
    if (ring_buf_space_get(&ring) >= needed) {
        ring_buf_put(&ring, header, 3);
        ring_buf_put(&ring, (uint8_t *)msg_buf, len);
    }
    /* If ring full, silently drop — better than crashing */
}

static void panic(const struct log_backend *const backend)
{
}

static void init(const struct log_backend *const backend)
{
    ring_buf_init(&ring, sizeof(ring_data), ring_data);
    ring_inited = true;
}

static const struct log_backend_api log_backend_iot_api = {
    .process = process,
    .panic = panic,
    .init = init,
};

LOG_BACKEND_DEFINE(log_backend_iot, log_backend_iot_api, true);

/* Called from iot_log_poll() in the main thread to drain queued messages. */
void iot_log_backend_drain(void)
{
    if (!ring_inited) {
        return;
    }

    uint8_t header[3];
    while (ring_buf_size_get(&ring) >= 3) {
        /* Peek header */
        if (ring_buf_peek(&ring, header, 3) < 3) {
            break;
        }

        uint8_t level = header[0];
        uint16_t len = ((uint16_t)header[1] << 8) | header[2];

        if (ring_buf_size_get(&ring) < (uint32_t)(3 + len)) {
            break; /* Incomplete entry — shouldn't happen */
        }

        /* Consume header */
        ring_buf_get(&ring, header, 3);

        /* Read message */
        char buf[500];
        uint16_t to_read = len > sizeof(buf) ? sizeof(buf) : len;
        ring_buf_get(&ring, (uint8_t *)buf, to_read);

        /* Discard remainder if message was truncated */
        if (len > to_read) {
            uint8_t discard[64];
            uint16_t remaining = len - to_read;
            while (remaining > 0) {
                uint16_t chunk = remaining > sizeof(discard) ? sizeof(discard) : remaining;
                ring_buf_get(&ring, discard, chunk);
                remaining -= chunk;
            }
        }

        /* Now send from the main thread context (safe stack) */
        iot_log_send_raw((iot_log_level_t)level, buf, to_read);
    }
}
