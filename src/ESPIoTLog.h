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
  // ESP-IDF Rich Debugging
  #include <esp_chip_info.h>
  #include <esp_flash.h>
  #include <esp_wifi.h>
  #include <freertos/task.h>
  #ifdef CONFIG_SECURE_BOOT_ENABLED
    #include <esp_secure_boot.h>
  #endif
  #ifdef CONFIG_FLASH_ENCRYPTION_ENABLED
    #include <esp_flash_encrypt.h>
  #endif
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

// Discovery interval with exponential backoff (configurable via #defines before including)
#ifndef DISCOVERY_INITIAL_INTERVAL
#define DISCOVERY_INITIAL_INTERVAL 0         // Start immediately on boot
#endif
#ifndef DISCOVERY_MIN_INTERVAL
#define DISCOVERY_MIN_INTERVAL 10000         // 10 seconds minimum
#endif
#ifndef DISCOVERY_MAX_INTERVAL
#define DISCOVERY_MAX_INTERVAL 300000        // 300 seconds (5 min) maximum
#endif
#ifndef DISCOVERY_BACKOFF_MULTIPLIER
#define DISCOVERY_BACKOFF_MULTIPLIER 2       // Double each time
#endif

// Legacy define for backwards compatibility
#define DEFAULT_DISCOVERY_INTERVAL DISCOVERY_MIN_INTERVAL

// Platform-specific buffer sizes
// Rationale: ESP8266 has ~50KB usable RAM vs ESP32's ~300KB
// Buffer sizes are optimized to balance functionality with memory constraints
#ifdef ESP8266
  #define DEFAULT_LOG_BUFFER_SIZE 256      // Message formatting buffer (halved for ESP8266)
  #define DEFAULT_MAX_MESSAGE_SIZE 128     // Max network message payload (halved for ESP8266)
  #define SERIAL_FORMAT_BUFFER_SIZE 128    // Serial output buffer (halved for ESP8266)
#else
  #define DEFAULT_LOG_BUFFER_SIZE 512      // Message formatting buffer (ESP32 full size)
  #define DEFAULT_MAX_MESSAGE_SIZE 256     // Max network message payload (ESP32 full size)
  #define SERIAL_FORMAT_BUFFER_SIZE 256    // Serial output buffer (ESP32 full size)
#endif

// Rate limiting configuration (optional - prevents log flooding)
#ifndef IOTLOG_RATE_LIMIT_ENABLED
  #define IOTLOG_RATE_LIMIT_ENABLED 1      // Enabled by default for safety
#endif
#ifndef IOTLOG_MAX_LOGS_PER_SECOND
  #define IOTLOG_MAX_LOGS_PER_SECOND 100   // Reasonable limit to prevent heap exhaustion
#endif

// Thread safety (opt-in for FreeRTOS multi-task scenarios)
// Note: Adds ~200-500 bytes RAM overhead per mutex + CPU cycles
#ifndef IOTLOG_THREAD_SAFE
  #define IOTLOG_THREAD_SAFE 0             // Disabled by default (most Arduino sketches are single-threaded)
#endif

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
    // ESP-IDF Rich Debugging (optional)
    TELEMETRY_ADVANCED_HEAP = 1 << 6,    // Per-capability heap breakdown
    TELEMETRY_TASK_INFO = 1 << 7,        // FreeRTOS task monitoring
    TELEMETRY_WIFI_ADVANCED = 1 << 8,    // Detailed WiFi diagnostics
    TELEMETRY_CHIP_INFO = 1 << 9,        // Hardware details (startup only)
    TELEMETRY_ALL = 0x3FF
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
    // Basic telemetry
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

    // ESP-IDF Advanced telemetry (optional)
    struct {
        uint32_t internal_free;
        uint32_t internal_largest;
        uint32_t external_free;     // PSRAM
        uint32_t external_largest;
        uint32_t dma_free;
        uint8_t task_count;
        uint16_t min_stack_remaining;
        char critical_task[16];     // Task with lowest stack
    } advanced;

    struct {
        uint8_t channel;
        int8_t tx_power;
        uint8_t bandwidth;          // 20/40MHz
        uint16_t beacon_timeout;
        uint8_t phy_mode;
        char country_code[4];
    } wifi_advanced;

    struct {
        uint8_t chip_model;
        uint8_t chip_cores;
        uint8_t chip_revision;
        uint32_t flash_size;
        uint8_t flash_mode;
        uint32_t cpu_freq_mhz;
        bool secure_boot;
        bool flash_encryption;
    } chip_info;
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
    void setSerialEnabled(bool enable);  // Control serial output (default: true)
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

    // ESP-IDF Rich debugging (one-time startup logging)
    void logStartupInfo();

    // Power management callbacks (optional - for battery-powered devices)
    // Set these callbacks to suspend/resume logging around sleep operations
    void (*onBeforeSleep)() = nullptr;
    void (*onAfterWake)() = nullptr;

    // Rate limiting control
    void resetRateLimit();  // Reset rate limiter counters

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

    // ESP-IDF Advanced telemetry collection
#ifdef ESP32
    void collectAdvancedHeap(SystemTelemetry& tel);
    void collectTaskInfo(SystemTelemetry& tel);
    void collectAdvancedWiFi(SystemTelemetry& tel);
    void collectChipInfo(SystemTelemetry& tel);
#endif

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
    bool _serial_enabled;
    uint32_t _discovery_interval;
    uint8_t _telemetry_flags;

    // Runtime state
    bool _initialized;
    bool _listener_active;
    bool _crash_logging_enabled;
    uint32_t _last_discovery;
    uint32_t _current_discovery_interval;  // Current interval (exponential backoff)
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

    // Rate limiting state (if enabled)
#if IOTLOG_RATE_LIMIT_ENABLED
    uint32_t _rate_limit_window_start;  // Start of current 1-second window
    uint16_t _rate_limit_count;         // Logs sent in current window
    uint32_t _rate_limit_dropped;       // Total logs dropped due to rate limit
#endif

    // Thread safety (if enabled)
#if IOTLOG_THREAD_SAFE
#ifdef ESP32
    portMUX_TYPE _mutex;                // ESP32 spinlock for critical sections
#endif
#endif

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