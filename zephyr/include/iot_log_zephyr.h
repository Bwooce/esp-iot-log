/*
 * ESP IoT Log - Zephyr/Thread Port
 * IPv6 multicast logging for Zephyr RTOS over OpenThread.
 * Wire-compatible with the Arduino/ESP-IDF libraries and Python receiver.
 *
 * Uses site-local multicast (ff05::e510) so packets cross the
 * Thread Border Router to the backbone LAN where the Python
 * receiver listens.
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

/* Protocol constants (must match Arduino/ESP-IDF libraries and Python receiver) */
#define IOT_LOG_MAGIC              0xE510
#define IOT_LOG_PROTOCOL_VERSION   1

/* Default IPv6 multicast — site-local scope so OTBR forwards to backbone.
 * The suffix 0xe510 matches our protocol magic number. */
#define IOT_LOG_DEFAULT_MCAST_IP6  "ff05::e510"
#define IOT_LOG_DEFAULT_MCAST_PORT 4210

/* Beacon discovery: Python receiver sends a beacon every 30s.
 * Device considers listener "active" if a beacon arrived within this window. */
#define IOT_LOG_BEACON_TYPE        0xFF
#define IOT_LOG_BEACON_TIMEOUT_S   90   /* 3x beacon interval */

/* Log levels (match Arduino/ESP-IDF libraries) */
typedef enum {
    IOT_LOG_NONE    = 0,
    IOT_LOG_ERROR   = 1,
    IOT_LOG_WARN    = 2,
    IOT_LOG_INFO    = 3,
    IOT_LOG_DEBUG   = 4,
    IOT_LOG_VERBOSE = 5,
} iot_log_level_t;

/* Log message types (match wire protocol) */
typedef enum {
    IOT_LOG_TYPE_TEXT      = 0x01,
    IOT_LOG_TYPE_TELEMETRY = 0x02,
    IOT_LOG_TYPE_EXCEPTION = 0x03,
    IOT_LOG_TYPE_METRIC    = 0x04,
} iot_log_type_t;

/* Configuration */
typedef struct {
    const char      *device_name;       /* NULL for auto (Thread EUI-64) */
    iot_log_level_t  level;             /* Minimum level to send */
    bool             serial_mirror;     /* Also output via Zephyr LOG (default true) */
    const char      *multicast_ip6;    /* NULL for default ff05::e510 */
    uint16_t         multicast_port;    /* 0 for default 4210 */
    bool             always_on;         /* Skip beacon discovery, always send (default false) */
} iot_log_config_t;

/* Default config initialiser */
#define IOT_LOG_CONFIG_DEFAULT() { \
    .device_name = NULL, \
    .level = IOT_LOG_INFO, \
    .serial_mirror = true, \
    .multicast_ip6 = NULL, \
    .multicast_port = 0, \
    .always_on = false, \
}

/**
 * Initialise the IoT log system.
 * Requires OpenThread to be attached to a network.
 * Creates a UDP6 socket for multicast transmission.
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
 * Call periodically to check for listener beacons.
 * In always_on mode this just checks Thread attachment.
 * In discovery mode (default) this checks for beacon packets
 * from the Python receiver. Should be called every few seconds.
 */
void iot_log_poll(void);

/**
 * Check if logging is active (listener discovered and network up).
 */
bool iot_log_is_active(void);

/**
 * Log at a specific level (printf-style).
 * Thread-safe — can be called from any Zephyr thread.
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
 * Get log statistics.
 */
uint32_t iot_log_get_sent_count(void);
uint32_t iot_log_get_dropped_count(void);

#ifdef __cplusplus
}
#endif
