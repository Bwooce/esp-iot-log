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
#include <zephyr/logging/log_ctrl.h>   /* log_source_name_get() */
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include <string.h>

/* Ring buffer for queued log messages.
 * Each entry: 1 byte level + 2 bytes length + message text.
 * 2KB holds ~10-20 typical log messages.
 *
 * Auto-initialised at compile time so it works regardless of whether the
 * backend's init() callback runs. With autostart=false, log_backend_enable()
 * doesn't actually invoke init(), so we can't rely on the runtime init path. */
#define IOT_LOG_RING_SIZE 2048
RING_BUF_DECLARE(ring, IOT_LOG_RING_SIZE);

/* Output buffer for log formatting (logging thread is single-threaded). */
static uint8_t log_buf[512];

/* Accumulation buffer for the current message. */
static char msg_buf[500];
static size_t msg_pos;

/* Diagnostic counters — exposed via log_backend_iot_get_stats() so the
 * periodic status emit in iot_log_zephyr.c can show why messages aren't
 * flowing when sent_count stays at 0. */
static uint32_t s_process_calls;
static uint32_t s_process_bail_inactive;
static uint32_t s_process_bail_no_fmt;
static uint32_t s_process_bail_zero_len;
static uint32_t s_process_queued;
static uint32_t s_process_ring_full;
static uint32_t s_process_bail_self;   /* dropped: source is our own transport */

/* Log sources whose messages we must NEVER rebroadcast, because doing so
 * would feed a send-failure feedback loop: this backend multicasts over the
 * OpenThread/IPv6 radio, and when that transport runs out of message buffers
 * it logs (from "net_otPlat_radio") "Cannot allocate new message buffer" /
 * "Error while calling otIp6Send". Queueing those for multicast makes the
 * drain thread try to send them over the very transport that just failed,
 * generating more NoBufs errors — a self-sustaining storm that starves MQTT.
 * Drop them here; the UART and web-ring backends still record them. The match
 * is by source name so it's a no-op on platforms without that source. */
static const char *const k_transport_sources[] = { "net_otPlat_radio" };

void log_backend_iot_get_stats(uint32_t *calls, uint32_t *bail_inactive,
                                uint32_t *bail_no_fmt, uint32_t *bail_zero_len,
                                uint32_t *queued, uint32_t *ring_full)
{
    if (calls) *calls = s_process_calls;
    if (bail_inactive) *bail_inactive = s_process_bail_inactive;
    if (bail_no_fmt) *bail_no_fmt = s_process_bail_no_fmt;
    if (bail_zero_len) *bail_zero_len = s_process_bail_zero_len;
    if (queued) *queued = s_process_queued;
    if (ring_full) *ring_full = s_process_ring_full;
}

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
    s_process_calls++;
    if (!iot_log_is_active()) {
        s_process_bail_inactive++;
        return;
    }

    /* Drop messages from our own transport's sources — rebroadcasting a
     * "no message buffer" error needs a message buffer, which feeds a NoBufs
     * storm. See k_transport_sources. UART/web backends still record them. */
    const void *src = log_msg_get_source(&msg->log);
    if (src != NULL) {
        const char *sname =
            log_source_name_get(log_msg_get_domain(&msg->log), log_source_id(src));
        if (sname != NULL) {
            for (size_t i = 0; i < ARRAY_SIZE(k_transport_sources); i++) {
                if (strcmp(sname, k_transport_sources[i]) == 0) {
                    s_process_bail_self++;
                    return;
                }
            }
        }
    }

    /* Format the message — skip level and timestamp since the binary
     * protocol header carries both. Keep module name for context. */
    msg_pos = 0;
    uint32_t flags = 0;
    log_format_func_t log_output_func = log_format_func_t_get(LOG_OUTPUT_TEXT);
    if (log_output_func == NULL) {
        /* Text formatter not available (CONFIG_LOG_OUTPUT_FORMAT_TEXT disabled) */
        s_process_bail_no_fmt++;
        return;
    }
    log_output_func(&log_output_iot, &msg->log, flags);

    if (msg_pos == 0) {
        s_process_bail_zero_len++;
        return;
    }

    /* Don't multicast OpenThread's per-frame transport chatter (radio MAC and
     * 6LoWPAN forwarding) at ANY log level. These come through log_generic()
     * with no Zephyr source, so they can't be source-filtered like
     * k_transport_sources — match OpenThread's region tag in the formatted
     * text "[<lvl>] <Region>-...". At INFO it's the per-tx-attempt firehose;
     * even at NOTE, MeshForwarder emits "Dropping rx (frag) frame" — and since
     * iot_log's own multicast packets are large and get 6LoWPAN-fragmented,
     * its traffic generates those very drops, which it would then re-multicast:
     * a second feedback path on top of the NoBufs one. Drop the whole region
     * from the multicast (it still reaches UART/web for local debug); Mle
     * state (attach/role/parent) and app logs still go out. */
    static const char *const k_drop_regions[] = { "Mac", "MeshForwarder" };
    if (msg_pos > 4 && msg_buf[0] == '[' && msg_buf[2] == ']' && msg_buf[3] == ' ') {
        const char *region = &msg_buf[4];
        size_t avail = msg_pos - 4;
        for (size_t i = 0; i < ARRAY_SIZE(k_drop_regions); i++) {
            size_t rlen = strlen(k_drop_regions[i]);
            if (avail >= rlen && memcmp(region, k_drop_regions[i], rlen) == 0) {
                s_process_bail_self++;
                return;
            }
        }
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
        s_process_queued++;
    } else {
        s_process_ring_full++;
    }
    /* If ring full, silently drop — better than crashing */
}

static void panic(const struct log_backend *const backend)
{
}

static const struct log_backend_api log_backend_iot_api = {
    .process = process,
    .panic = panic,
    /* No init — RING_BUF_DECLARE handles ring init at compile time, and
     * log_backend_enable() doesn't invoke init() anyway. */
};

/* autostart=false: caller must enable via log_backend_enable() once
 * iot_log_init() has run AND networking (Thread) is up. Otherwise this
 * backend's process() returns silently when iot_log isn't active —
 * which CONSUMES the message from the deferred-log subsystem (refcount
 * decremented, message freed) before any other backend (e.g. UART)
 * gets a chance to deliver it. With autostart off, messages stay
 * queued for delivery once a working backend is enabled.
 *
 * Activation happens in iot_log_init() (iot_log_zephyr.c). */
LOG_BACKEND_DEFINE(log_backend_iot, log_backend_iot_api, false);

/* Called from iot_log_poll() in the main thread to drain queued messages. */
void iot_log_backend_drain(void)
{
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
