/**
 * ESP IoT Log - Multicast logging with mDNS discovery
 * Copyright (c) 2026 Bruce Fitzsimons
 *
 * Lightweight logging library for ESP32/ESP8266 that uses mDNS discovery
 * to detect active listeners before sending multicast UDP logs.
 * Zero network overhead when no monitoring is active.
 */

#ifndef ESP_IOT_LOG_H
#define ESP_IOT_LOG_H

#include <Arduino.h>
#ifdef ESP32
  #include <WiFi.h>
  #include <WiFiUdp.h>
  #include <ESPmDNS.h>
  #include <esp_system.h>
  #include <esp_heap_caps.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <WiFiUdp.h>
  #include <ESP8266mDNS.h>
  #include <user_interface.h>
#endif
#include "ESPCrashHandler.h"

// Library version
#define ESP_IOT_LOG_VERSION "1.0.0"

// Default configuration
#define DEFAULT_MULTICAST_IP "239.255.1.100"
#define DEFAULT_MULTICAST_PORT 4210
#define DEFAULT_SERVICE_NAME "_esp-iot-log._udp.local"
#define DEFAULT_DISCOVERY_INTERVAL 10000  // 10 seconds
#define DEFAULT_LOG_BUFFER_SIZE 512
#define DEFAULT_MAX_MESSAGE_SIZE 256

// Log level masks for selective filtering
#define LOG_MASK_NONE     0x00
#define LOG_MASK_ERROR    0x01
#define LOG_MASK_WARN     0x02
#define LOG_MASK_INFO     0x04
#define LOG_MASK_DEBUG    0x08
#define LOG_MASK_VERBOSE  0x10
#define LOG_MASK_ALL      0xFF

// Log levels
typedef enum {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_INFO = 3,
    LOG_LEVEL_DEBUG = 4,
    LOG_LEVEL_VERBOSE = 5
} log_level_t;

// System telemetry flags
typedef enum {
    TELEMETRY_NONE = 0,
    TELEMETRY_HEAP = 1 << 0,
    TELEMETRY_WIFI = 1 << 1,
    TELEMETRY_TEMPERATURE = 1 << 2,
    TELEMETRY_STACK = 1 << 3,
    TELEMETRY_FLASH = 1 << 4,
    TELEMETRY_RESET = 1 << 5,
    TELEMETRY_ALL = 0xFF
} telemetry_flags_t;

// Log message types
typedef enum {
    LOG_TYPE_TEXT = 0x01,
    LOG_TYPE_TELEMETRY = 0x02,
    LOG_TYPE_EXCEPTION = 0x03,
    LOG_TYPE_METRIC = 0x04
} log_type_t;

// Log message header (packed)
struct __attribute__((packed)) LogHeader {
    uint16_t magic;         // 0xE510 (ESP + LOG)
    uint8_t version;        // Protocol version
    uint64_t device_id;     // Device MAC as uint64
    uint32_t timestamp;     // Millis since boot
    uint8_t log_type;       // LogType enum
    uint16_t length;        // Payload length
};

// System telemetry data
struct SystemTelemetry {
    uint32_t heap_free;
    uint32_t heap_largest_block;
    uint8_t heap_fragmentation;
    int8_t wifi_rssi;
    uint8_t wifi_status;
    uint16_t wifi_reconnects;
    int16_t temperature;    // Celsius * 10
    uint8_t reset_reason;
    uint32_t uptime;
    uint16_t free_stack;
};

class ESPIoTLog {
public:
    ESPIoTLog();
    ~ESPIoTLog();

    // Initialization
    bool begin(const char* device_name = nullptr, log_level_t level = LOG_LEVEL_INFO);
    void end();

    // Configuration
    void setLogLevel(log_level_t level);
    void setLogMask(uint8_t mask);  // Bitmask to enable/disable specific levels
    void setMulticastAddress(const char* ip, uint16_t port);
    void setDiscoveryInterval(uint32_t interval_ms);
    void setServiceName(const char* service);
    void enableSystemTelemetry(uint8_t flags);

    // Logging methods
    void error(const char* format, ...);
    void warn(const char* format, ...);
    void info(const char* format, ...);
    void debug(const char* format, ...);
    void verbose(const char* format, ...);

    // Direct logging with level
    void log(log_level_t level, const char* format, ...);
    void logf(log_level_t level, const char* format, va_list args);

    // Custom metrics
    void logMetric(const char* name, int32_t value);
    void logMetric(const char* name, const char* value);

    // System state
    bool isListenerActive() const { return _listener_active; }
    uint32_t getLogCount() const { return _log_count; }
    uint32_t getDroppedCount() const { return _dropped_count; }

    // Manual operations
    void forceDiscovery();
    void sendTelemetry();

    // Crash handling
    void enableCrashLogging(bool enable = true);
    void checkAndLogCrashes();
    void logCrash(const char* reason);

    // Task loop (call from main loop)
    void loop();

private:
    // Core functionality
    bool discoverListener();
    bool sendLogMessage(log_type_t type, const uint8_t* data, size_t length);
    void formatLogMessage(log_level_t level, const char* message);
    void collectTelemetry(SystemTelemetry& tel);
    uint64_t getDeviceId();
    uint16_t calculateChecksum(const uint8_t* data, size_t length);

    // State management
    void resetDiscovery();
    void updateListenerStatus();

    // Configuration
    char _device_name[32];
    char _multicast_ip[16];
    uint16_t _multicast_port;
    char _service_name[64];
    log_level_t _log_level;
    uint8_t _log_mask;
    uint32_t _discovery_interval;
    uint8_t _telemetry_flags;

    // Runtime state
    bool _initialized;
    bool _listener_active;
    bool _crash_logging_enabled;
    uint32_t _last_discovery;
    uint32_t _last_telemetry;
    uint32_t _log_count;
    uint32_t _dropped_count;
    uint64_t _device_id;

    // Network objects
    WiFiUDP _udp;
    IPAddress _multicast_addr;

    // Buffer management
    char _log_buffer[DEFAULT_LOG_BUFFER_SIZE];
    size_t _buffer_size;

    // Constants
    static const uint16_t LOG_MAGIC = 0xE510;
    static const uint8_t PROTOCOL_VERSION = 1;
    static const uint32_t TELEMETRY_INTERVAL = 30000; // 30 seconds
};

// Global instance (optional convenience)
extern ESPIoTLog iotlog;

// Convenience macros
#define LOG_ERROR(...)   iotlog.error(__VA_ARGS__)
#define LOG_WARN(...)    iotlog.warn(__VA_ARGS__)
#define LOG_INFO(...)    iotlog.info(__VA_ARGS__)
#define LOG_DEBUG(...)   iotlog.debug(__VA_ARGS__)
#define LOG_VERBOSE(...) iotlog.verbose(__VA_ARGS__)

#endif // ESP_IOT_LOG_H