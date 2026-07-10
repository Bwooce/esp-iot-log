/*
 * ESP IoT Log - Pure ESP-IDF C port
 * Multicast UDP logging with mDNS discovery.
 * Zero network overhead when no monitoring is active.
 *
 * Wire-compatible with the Arduino ESPIoTLog library and its Python receiver.
 *
 * Original: https://github.com/Bwooce/esp-iot-log
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol constants (must match Arduino library and Python receiver) */
#define IOT_LOG_MAGIC              0xE510
#define IOT_LOG_PROTOCOL_VERSION   1
#define IOT_LOG_DEFAULT_MCAST_IP   "239.255.1.100"
#define IOT_LOG_DEFAULT_MCAST_PORT 4210
#define IOT_LOG_SERVICE_NAME       "_esp-iot-log"
#define IOT_LOG_SERVICE_PROTO      "_udp"

/* Log levels (match Arduino library) */
typedef enum {
    IOT_LOG_NONE    = 0,
    IOT_LOG_ERROR   = 1,
    IOT_LOG_WARN    = 2,
    IOT_LOG_INFO    = 3,
    IOT_LOG_DEBUG   = 4,
    IOT_LOG_VERBOSE = 5,
} iot_log_level_t;

/* Log message types (match Arduino library wire protocol) */
typedef enum {
    IOT_LOG_TYPE_TEXT      = 0x01,
    IOT_LOG_TYPE_TELEMETRY = 0x02,
    IOT_LOG_TYPE_EXCEPTION = 0x03,
    IOT_LOG_TYPE_METRIC    = 0x04,
} iot_log_type_t;

/* Configuration */
typedef struct {
    const char      *device_name;       /* NULL for auto (ESP-<mac>) */
    iot_log_level_t  level;             /* Minimum level to send */
    bool             serial_mirror;     /* Also output via ESP_LOG (default true) */
    const char      *multicast_ip;      /* NULL for default 239.255.1.100 */
    uint16_t         multicast_port;    /* 0 for default 4210 */
    uint32_t         discovery_interval_ms;  /* 0 for default 60s */
    bool             redirect_esp_log;  /* Hook ESP_LOGx to also send via iot_log */
    /* Unicast override: when non-NULL/non-empty, ALSO send every log/metric as a
     * unicast UDP datagram to this host on multicast_port (default 4210), with NO
     * mDNS-listener gate (unicast doesn't need discovery). This is the working path
     * on boards where the Wi-Fi transport drops multicast TX (esp_hosted/C6 — see
     * memory/project_esp_hosted_multicast_tx_broken). Multicast still fires too when
     * a listener is discovered; the receiver accepts both on one socket. */
    const char      *unicast_ip;        /* NULL/"" = multicast-only (legacy behaviour) */
    /* Skip mDNS entirely: don't call mdns_init() and never run listener discovery.
     * Saves the ~4 KB mDNS task stack + buffers and the periodic discovery CPU on
     * boards where multicast TX is dropped anyway (so discovery can never succeed) —
     * use with unicast_ip for a working, mDNS-free telemetry path. */
    bool             disable_mdns;      /* true = no mdns_init(), no discovery */
} iot_log_config_t;

/* Default config initialiser */
#define IOT_LOG_CONFIG_DEFAULT() { \
    .device_name = NULL, \
    .level = IOT_LOG_INFO, \
    .serial_mirror = true, \
    .multicast_ip = NULL, \
    .multicast_port = 0, \
    .discovery_interval_ms = 0, \
    .redirect_esp_log = false, \
    .unicast_ip = NULL, \
    .disable_mdns = false, \
}

/**
 * Initialise the IoT log system. Requires WiFi to be connected.
 * Sets up mDNS discovery and UDP multicast socket.
 *
 * @param config  Configuration (NULL for all defaults)
 * @return 0 on success, -1 on error
 */
int iot_log_init(const iot_log_config_t *config);

/**
 * Deinitialise and release resources.
 */
void iot_log_deinit(void);

/**
 * Call periodically (e.g. from a task loop) to perform mDNS discovery
 * and send telemetry. Should be called at least every few seconds.
 */
void iot_log_poll(void);

/**
 * Check if a network listener is currently active.
 */
bool iot_log_listener_active(void);

/**
 * Log at a specific level (printf-style).
 */
void iot_log(iot_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Convenience macros */
#define IOT_LOGE(fmt, ...) iot_log(IOT_LOG_ERROR,   fmt, ##__VA_ARGS__)
#define IOT_LOGW(fmt, ...) iot_log(IOT_LOG_WARN,    fmt, ##__VA_ARGS__)
#define IOT_LOGI(fmt, ...) iot_log(IOT_LOG_INFO,    fmt, ##__VA_ARGS__)
#define IOT_LOGD(fmt, ...) iot_log(IOT_LOG_DEBUG,   fmt, ##__VA_ARGS__)
#define IOT_LOGV(fmt, ...) iot_log(IOT_LOG_VERBOSE, fmt, ##__VA_ARGS__)

/**
 * Log a numeric metric.
 */
void iot_log_metric(const char *name, int32_t value);

/**
 * Log a string metric.
 */
void iot_log_metric_str(const char *name, const char *value);

/**
 * Check for a core dump from a previous crash and report it.
 * Logs crash details (task, PC, panic reason, backtrace) to serial
 * via ESP_LOGW and over the network via iot_log ERROR messages.
 * Erases the core dump from flash after reporting.
 *
 * Can be called before or after iot_log_init(). Serial logging always
 * works; network logging requires init + active listener.
 *
 * Requires CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH. Returns false (no-op)
 * when core dumps are not enabled or not stored to flash.
 *
 * Compatible with ESP-IDF v4.x, v5.x, and v6.x:
 *  - v4.x/v5.0-5.1: summary only (no panic reason string)
 *  - v5.2+: summary + panic reason string
 *  - v6.0+: ELF is the only format, no format guard needed
 *
 * @return true if a core dump was found and reported, false otherwise
 */
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
bool iot_log_check_coredump(void);
#else
static inline bool iot_log_check_coredump(void) { return false; }
#endif

/**
 * Force an immediate mDNS discovery (resets backoff timer).
 */
void iot_log_force_discovery(void);

/**
 * Get log statistics.
 */
uint32_t iot_log_get_sent_count(void);
uint32_t iot_log_get_dropped_count(void);

#ifdef __cplusplus
}
#endif
