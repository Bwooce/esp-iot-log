/*
 * ESP IoT Log - Pure ESP-IDF C port
 * Wire-compatible with the Arduino ESPIoTLog library.
 *
 * Uses mDNS discovery to detect listeners before sending multicast UDP logs.
 * Zero network overhead when no monitoring is active.
 */

#include "esp_iot_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#include "esp_idf_version.h"
#endif

static const char *TAG = "iot_log";

/* Discovery backoff parameters (match Arduino library) */
#define DISCOVERY_MIN_INTERVAL_MS   60000
#define DISCOVERY_MAX_INTERVAL_MS  300000
#define DISCOVERY_BACKOFF_MULT     2

/* Internal state */
static struct {
    bool            initialised;
    bool            listener_active;
    iot_log_level_t level;
    bool            serial_mirror;

    int             sock;
    struct sockaddr_in mcast_addr;

    uint64_t        device_id;
    char            device_name[32];

    int64_t         last_discovery_us;
    uint32_t        discovery_interval_ms;

    uint32_t        sent_count;
    uint32_t        dropped_count;
} s_log;

/* CRC16-CCITT (must match Arduino library and Python receiver) */
static uint16_t crc16_ccitt(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* Send a raw log message using the binary protocol */
static bool send_message(iot_log_type_t type, const uint8_t *payload, size_t payload_len)
{
    if (!s_log.initialised || !s_log.listener_active) {
        s_log.dropped_count++;
        return false;
    }

    /* Header: magic(2) + version(1) + device_id(8) + timestamp(4) + type(1) + length(2) = 18 */
    const size_t HEADER_SIZE = 18;
    size_t total_size = HEADER_SIZE + payload_len + 2; /* +2 for CRC16 */
    if (total_size > 1024) {
        s_log.dropped_count++;
        return false;
    }

    uint8_t packet[1024];
    size_t pos = 0;

    /* Magic (little-endian) */
    packet[pos++] = IOT_LOG_MAGIC & 0xFF;
    packet[pos++] = (IOT_LOG_MAGIC >> 8) & 0xFF;

    /* Protocol version */
    packet[pos++] = IOT_LOG_PROTOCOL_VERSION;

    /* Device ID (little-endian, 8 bytes) */
    uint64_t did = s_log.device_id;
    for (int i = 0; i < 8; i++) {
        packet[pos++] = did & 0xFF;
        did >>= 8;
    }

    /* Timestamp in ms (little-endian, 4 bytes) */
    uint32_t ts_ms = (uint32_t)(esp_timer_get_time() / 1000);
    packet[pos++] = ts_ms & 0xFF;
    packet[pos++] = (ts_ms >> 8) & 0xFF;
    packet[pos++] = (ts_ms >> 16) & 0xFF;
    packet[pos++] = (ts_ms >> 24) & 0xFF;

    /* Log type */
    packet[pos++] = (uint8_t)type;

    /* Payload length (little-endian, 2 bytes) */
    packet[pos++] = payload_len & 0xFF;
    packet[pos++] = (payload_len >> 8) & 0xFF;

    /* Payload */
    if (payload_len > 0) {
        memcpy(packet + pos, payload, payload_len);
    }

    /* CRC16 over header + payload */
    uint16_t crc = crc16_ccitt(packet, HEADER_SIZE + payload_len);
    packet[HEADER_SIZE + payload_len]     = crc & 0xFF;
    packet[HEADER_SIZE + payload_len + 1] = (crc >> 8) & 0xFF;

    ssize_t sent = sendto(s_log.sock, packet, total_size, 0,
                          (struct sockaddr *)&s_log.mcast_addr,
                          sizeof(s_log.mcast_addr));
    if (sent == (ssize_t)total_size) {
        s_log.sent_count++;
        return true;
    } else {
        s_log.dropped_count++;
        return false;
    }
}

/* Discover listener via mDNS */
static bool discover_listener(void)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(IOT_LOG_SERVICE_NAME, IOT_LOG_SERVICE_PROTO,
                                    1000, 1, &results);
    bool found = (err == ESP_OK && results != NULL);

    if (results) {
        mdns_query_results_free(results);
    }

    if (found != s_log.listener_active) {
        s_log.listener_active = found;
        if (found) {
            ESP_LOGI(TAG, "Log listener discovered");
        } else {
            ESP_LOGD(TAG, "Log listener not found");
        }
    }

    return found;
}

int iot_log_init(const iot_log_config_t *config)
{
    if (s_log.initialised) {
        return 0;
    }

    memset(&s_log, 0, sizeof(s_log));
    s_log.sock = -1;

    /* Apply config or defaults */
    iot_log_config_t cfg = IOT_LOG_CONFIG_DEFAULT();
    if (config) {
        cfg = *config;
    }

    s_log.level = cfg.level;
    s_log.serial_mirror = cfg.serial_mirror;
    s_log.discovery_interval_ms = cfg.discovery_interval_ms > 0 ?
        cfg.discovery_interval_ms : DISCOVERY_MIN_INTERVAL_MS;

    /* Get device MAC as ID */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s_log.device_id = 0;
    for (int i = 5; i >= 0; i--) {
        s_log.device_id = (s_log.device_id << 8) | mac[i];
    }

    /* Device name */
    if (cfg.device_name) {
        strncpy(s_log.device_name, cfg.device_name, sizeof(s_log.device_name) - 1);
    } else {
        snprintf(s_log.device_name, sizeof(s_log.device_name),
                 "ESP-%02X%02X%02X", mac[3], mac[4], mac[5]);
    }

    /* Initialise mDNS (may already be running) */
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* Create UDP socket */
    s_log.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_log.sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return -1;
    }

    /* Make socket non-blocking so logging never stalls */
    int flags = fcntl(s_log.sock, F_GETFL, 0);
    fcntl(s_log.sock, F_SETFL, flags | O_NONBLOCK);

    /* Set up multicast destination */
    const char *mcast_ip = cfg.multicast_ip ? cfg.multicast_ip : IOT_LOG_DEFAULT_MCAST_IP;
    uint16_t mcast_port = cfg.multicast_port > 0 ? cfg.multicast_port : IOT_LOG_DEFAULT_MCAST_PORT;

    memset(&s_log.mcast_addr, 0, sizeof(s_log.mcast_addr));
    s_log.mcast_addr.sin_family = AF_INET;
    s_log.mcast_addr.sin_port = htons(mcast_port);
    inet_pton(AF_INET, mcast_ip, &s_log.mcast_addr.sin_addr);

    /* Set multicast TTL to 1 (stay on local network) */
    int ttl = 1;
    setsockopt(s_log.sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    s_log.initialised = true;
    s_log.last_discovery_us = 0; /* Force immediate discovery on first poll */

    ESP_LOGI(TAG, "Initialised: %s -> %s:%d", s_log.device_name, mcast_ip, mcast_port);
    return 0;
}

void iot_log_deinit(void)
{
    if (!s_log.initialised) return;

    if (s_log.sock >= 0) {
        close(s_log.sock);
        s_log.sock = -1;
    }

    s_log.initialised = false;
    s_log.listener_active = false;
}

void iot_log_poll(void)
{
    if (!s_log.initialised) return;

    int64_t now = esp_timer_get_time();
    uint32_t effective_interval_ms = s_log.listener_active ?
        DISCOVERY_MAX_INTERVAL_MS : s_log.discovery_interval_ms;

    int64_t elapsed_us = now - s_log.last_discovery_us;
    if (s_log.last_discovery_us == 0 || elapsed_us >= (int64_t)effective_interval_ms * 1000) {
        bool found = discover_listener();
        s_log.last_discovery_us = esp_timer_get_time();

        if (!found) {
            s_log.discovery_interval_ms *= DISCOVERY_BACKOFF_MULT;
            if (s_log.discovery_interval_ms > DISCOVERY_MAX_INTERVAL_MS) {
                s_log.discovery_interval_ms = DISCOVERY_MAX_INTERVAL_MS;
            }
            if (s_log.discovery_interval_ms < DISCOVERY_MIN_INTERVAL_MS) {
                s_log.discovery_interval_ms = DISCOVERY_MIN_INTERVAL_MS;
            }
        } else {
            s_log.discovery_interval_ms = DISCOVERY_MIN_INTERVAL_MS;
        }
    }
}

bool iot_log_listener_active(void)
{
    return s_log.listener_active;
}

void iot_log(iot_log_level_t level, const char *fmt, ...)
{
    if (!s_log.initialised || level > s_log.level) return;

    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* Mirror to ESP_LOG if enabled */
    if (s_log.serial_mirror) {
        switch (level) {
            case IOT_LOG_ERROR:   ESP_LOGE(TAG, "%s", msg); break;
            case IOT_LOG_WARN:    ESP_LOGW(TAG, "%s", msg); break;
            case IOT_LOG_INFO:    ESP_LOGI(TAG, "%s", msg); break;
            case IOT_LOG_DEBUG:   ESP_LOGD(TAG, "%s", msg); break;
            case IOT_LOG_VERBOSE: ESP_LOGV(TAG, "%s", msg); break;
            default: break;
        }
    }

    /* Send over network if listener active */
    if (s_log.listener_active) {
        size_t msg_len = strlen(msg);
        if (msg_len > 500) msg_len = 500;

        uint8_t payload[502];
        payload[0] = (uint8_t)level;
        memcpy(payload + 1, msg, msg_len);

        send_message(IOT_LOG_TYPE_TEXT, payload, 1 + msg_len);
    }
}

void iot_log_metric(const char *name, int32_t value)
{
    if (!s_log.initialised || !s_log.listener_active) return;
    if (!name || name[0] == '\0') return;

    size_t name_len = strlen(name);
    if (name_len > 255) name_len = 255;

    uint8_t payload[260];
    payload[0] = (uint8_t)name_len;
    memcpy(payload + 1, name, name_len);
    /* Value as little-endian 4 bytes */
    payload[1 + name_len]     = (uint8_t)(value);
    payload[1 + name_len + 1] = (uint8_t)(value >> 8);
    payload[1 + name_len + 2] = (uint8_t)(value >> 16);
    payload[1 + name_len + 3] = (uint8_t)(value >> 24);

    send_message(IOT_LOG_TYPE_METRIC, payload, 1 + name_len + 4);
}

void iot_log_metric_str(const char *name, const char *value)
{
    if (!s_log.initialised || !s_log.listener_active) return;
    if (!name || name[0] == '\0') return;
    if (!value) value = "";

    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    if (name_len > 255) name_len = 255;
    if (value_len > 255) value_len = 255;

    uint8_t payload[514];
    payload[0] = (uint8_t)name_len;
    memcpy(payload + 1, name, name_len);
    payload[1 + name_len] = (uint8_t)value_len;
    memcpy(payload + 2 + name_len, value, value_len);

    send_message(IOT_LOG_TYPE_METRIC, payload, 2 + name_len + value_len);
}

void iot_log_force_discovery(void)
{
    s_log.last_discovery_us = 0;
    s_log.discovery_interval_ms = DISCOVERY_MIN_INTERVAL_MS;
}

uint32_t iot_log_get_sent_count(void)
{
    return s_log.sent_count;
}

uint32_t iot_log_get_dropped_count(void)
{
    return s_log.dropped_count;
}

/*
 * Core dump check, report, and erase.
 *
 * IDF compatibility:
 *  - esp_core_dump_get_summary() requires ELF format in IDF < 6.0
 *    (guarded by CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF).
 *    IDF 6.0 only supports ELF, so no format guard needed.
 *  - esp_core_dump_get_panic_reason() was added in IDF 5.2.
 *  - image_check / image_erase are available in all versions (4.x+).
 */
#ifdef CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH

/* Can we call esp_core_dump_get_summary()? */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  /* IDF 6.0+: ELF is the only format, summary always available */
  #define IOT_LOG_HAVE_COREDUMP_SUMMARY 1
#elif defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
  /* IDF 4.x / 5.x: summary only available with ELF format */
  #define IOT_LOG_HAVE_COREDUMP_SUMMARY 1
#else
  #define IOT_LOG_HAVE_COREDUMP_SUMMARY 0
#endif

/* Can we call esp_core_dump_get_panic_reason()? (added in IDF 5.2) */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
  #define IOT_LOG_HAVE_PANIC_REASON 1
#else
  #define IOT_LOG_HAVE_PANIC_REASON 0
#endif

bool iot_log_check_coredump(void)
{
    esp_err_t err = esp_core_dump_image_check();
    if (err != ESP_OK) {
        return false;
    }

    ESP_LOGW(TAG, "-- Previous crash detected --");

#if IOT_LOG_HAVE_COREDUMP_SUMMARY
    /* Allocate on heap -- esp_core_dump_summary_t is ~200+ bytes */
    esp_core_dump_summary_t *summary = calloc(1, sizeof(esp_core_dump_summary_t));
    if (summary == NULL) {
        ESP_LOGE(TAG, "Failed to allocate coredump summary");
        goto erase;
    }

    err = esp_core_dump_get_summary(summary);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read coredump summary: %s", esp_err_to_name(err));
        free(summary);
        goto erase;
    }

    ESP_LOGW(TAG, "Task: %s  PC: 0x%08lx  Cause: %lu",
             summary->exc_task,
             (unsigned long)summary->exc_pc,
             (unsigned long)summary->ex_info.exc_cause);

    /* Log backtrace (up to 16 PCs) */
    if (summary->exc_bt_info.depth > 0) {
        char bt_buf[256];
        int pos = 0;
        uint32_t depth = summary->exc_bt_info.depth;
        if (depth > 16) depth = 16;

        for (uint32_t i = 0; i < depth && pos < (int)sizeof(bt_buf) - 12; i++) {
            pos += snprintf(bt_buf + pos, sizeof(bt_buf) - pos,
                           "%s0x%08lx", i > 0 ? " " : "",
                           (unsigned long)summary->exc_bt_info.bt[i]);
        }
        if (summary->exc_bt_info.corrupted) {
            pos += snprintf(bt_buf + pos, sizeof(bt_buf) - pos, " [corrupted]");
        }
        ESP_LOGW(TAG, "Backtrace: %s", bt_buf);
    }
#endif /* IOT_LOG_HAVE_COREDUMP_SUMMARY */

#if IOT_LOG_HAVE_PANIC_REASON
    char reason[200];
    err = esp_core_dump_get_panic_reason(reason, sizeof(reason));
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Reason: %s", reason);
    }
#endif

    /* Best-effort network send via iot_log (works if init'd + listener active) */
#if IOT_LOG_HAVE_COREDUMP_SUMMARY
    if (summary != NULL) {
        iot_log(IOT_LOG_ERROR, "CRASH: task=%s pc=0x%08lx cause=%lu",
                summary->exc_task,
                (unsigned long)summary->exc_pc,
                (unsigned long)summary->ex_info.exc_cause);

        if (summary->exc_bt_info.depth > 0) {
            char bt_buf[256];
            int pos = 0;
            uint32_t depth = summary->exc_bt_info.depth;
            if (depth > 16) depth = 16;

            for (uint32_t i = 0; i < depth && pos < (int)sizeof(bt_buf) - 12; i++) {
                pos += snprintf(bt_buf + pos, sizeof(bt_buf) - pos,
                               "%s0x%08lx", i > 0 ? " " : "",
                               (unsigned long)summary->exc_bt_info.bt[i]);
            }
            iot_log(IOT_LOG_ERROR, "BACKTRACE: %s%s", bt_buf,
                    summary->exc_bt_info.corrupted ? " [corrupted]" : "");
        }

        free(summary);
    }
#endif /* IOT_LOG_HAVE_COREDUMP_SUMMARY */

#if IOT_LOG_HAVE_PANIC_REASON
    /* Re-read reason for network send (summary may have been freed) */
    {
        char reason_net[200];
        if (esp_core_dump_get_panic_reason(reason_net, sizeof(reason_net)) == ESP_OK) {
            iot_log(IOT_LOG_ERROR, "REASON: %s", reason_net);
        }
    }
#endif

#if IOT_LOG_HAVE_COREDUMP_SUMMARY
erase:
#endif
    esp_core_dump_image_erase();
    ESP_LOGW(TAG, "Core dump erased from flash");
    return true;
}

#endif /* CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH */
