/**
 * ESP IoT Log - Implementation
 * Copyright (c) 2026 Bruce Fitzsimons
 */

#include "ESPIoTLog.h"
#include <stdarg.h>
#include <stdio.h>

// Global instance
ESPIoTLog iotlog;

// Log level strings
static const char* LOG_LEVEL_NAMES[] = {
    "NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"
};

ESPIoTLog::ESPIoTLog() :
    _multicast_port(DEFAULT_MULTICAST_PORT),
    _log_level(LOG_LEVEL_INFO),
    _log_mask(LOG_MASK_ALL),
    _serial_enabled(true),
    _discovery_interval(DISCOVERY_MIN_INTERVAL),
    _telemetry_flags(TELEMETRY_NONE),
    _initialized(false),
    _listener_active(false),
    _crash_logging_enabled(false),
    _last_discovery(0),
    _current_discovery_interval(DISCOVERY_INITIAL_INTERVAL),
    _last_telemetry(0),
    _log_count(0),
    _dropped_count(0),
    _device_id(0),
    _buffer_size(DEFAULT_LOG_BUFFER_SIZE)
{
    strncpy(_multicast_ip, DEFAULT_MULTICAST_IP, sizeof(_multicast_ip));
    strncpy(_service_name, DEFAULT_SERVICE_NAME, sizeof(_service_name));
    _device_name[0] = '\0';
}

ESPIoTLog::~ESPIoTLog() {
    end();
}

bool ESPIoTLog::begin(const char* device_name, log_level_t level) {
    if (_initialized) {
        return true;
    }

    // Set device name
    if (device_name) {
        strncpy(_device_name, device_name, sizeof(_device_name) - 1);
        _device_name[sizeof(_device_name) - 1] = '\0';
    } else {
#ifdef ESP32
        snprintf(_device_name, sizeof(_device_name), "ESP-%llX", ESP.getEfuseMac());
#elif defined(ESP8266)
        snprintf(_device_name, sizeof(_device_name), "ESP-%08X", ESP.getChipId());
#endif
    }

    _log_level = level;
    _device_id = getDeviceId();

    // Parse multicast IP
    if (!_multicast_addr.fromString(_multicast_ip)) {
        Serial.println("ESPIoTLog: Invalid multicast IP");
        return false;
    }

    // Initialize mDNS only if not already running
    bool mdns_started = false;
#ifdef ESP32
    // Try to query mDNS to see if it's already running
    int test_result = MDNS.queryService("test", "tcp");
    mdns_started = (test_result >= 0); // -1 indicates mDNS not initialized
#elif defined(ESP8266)
    // ESP8266: Check if mDNS is already running by checking instance
    mdns_started = MDNS.isRunning();
#endif

    if (!mdns_started) {
        if (!MDNS.begin(_device_name)) {
            Serial.println("ESPIoTLog: Failed to start mDNS");
            return false;
        }
    }

    // Initialize UDP socket for sending multicast packets
    if (!_udp.begin(0)) {  // Use any available port for sending
        Serial.println("ESPIoTLog: Failed to initialize UDP");
        return false;
    }

    _initialized = true;

    info("ESPIoTLog initialized: %s", _device_name);

    // Check for crash data from previous boot
    checkAndLogCrashes();

    return true;
}

void ESPIoTLog::end() {
    if (!_initialized) return;

    _udp.stop();
    _initialized = false;
    _listener_active = false;
}

void ESPIoTLog::setLogLevel(log_level_t level) {
    _log_level = level;
}

void ESPIoTLog::setLogMask(uint8_t mask) {
    _log_mask = mask;
}

void ESPIoTLog::setSerialEnabled(bool enable) {
    _serial_enabled = enable;
}

void ESPIoTLog::setMulticastAddress(const char* ip, uint16_t port) {
    if (!ip || strlen(ip) == 0) {
        return;  // Invalid IP, ignore
    }

    strncpy(_multicast_ip, ip, sizeof(_multicast_ip) - 1);
    _multicast_ip[sizeof(_multicast_ip) - 1] = '\0';
    _multicast_port = port;

    // Validate IP format
    if (!_multicast_addr.fromString(_multicast_ip)) {
        // Failed to parse, revert to default
        strncpy(_multicast_ip, DEFAULT_MULTICAST_IP, sizeof(_multicast_ip) - 1);
        _multicast_addr.fromString(_multicast_ip);
    }
}

void ESPIoTLog::setDiscoveryInterval(uint32_t interval_ms) {
    _discovery_interval = interval_ms;
}

void ESPIoTLog::setServiceName(const char* service) {
    if (!service || strlen(service) == 0) {
        return;  // Invalid service name, ignore
    }

    strncpy(_service_name, service, sizeof(_service_name) - 1);
    _service_name[sizeof(_service_name) - 1] = '\0';
}

void ESPIoTLog::enableSystemTelemetry(uint8_t flags) {
    _telemetry_flags = flags;
}

void ESPIoTLog::error(const char* format, ...) {
    if (_log_level >= LOG_LEVEL_ERROR) {
        va_list args;
        va_start(args, format);
        logf(LOG_LEVEL_ERROR, format, args);
        va_end(args);
    }
}

void ESPIoTLog::warn(const char* format, ...) {
    if (_log_level >= LOG_LEVEL_WARN) {
        va_list args;
        va_start(args, format);
        logf(LOG_LEVEL_WARN, format, args);
        va_end(args);
    }
}

void ESPIoTLog::info(const char* format, ...) {
    if (_log_level >= LOG_LEVEL_INFO) {
        va_list args;
        va_start(args, format);
        logf(LOG_LEVEL_INFO, format, args);
        va_end(args);
    }
}

void ESPIoTLog::debug(const char* format, ...) {
    if (_log_level >= LOG_LEVEL_DEBUG) {
        va_list args;
        va_start(args, format);
        logf(LOG_LEVEL_DEBUG, format, args);
        va_end(args);
    }
}

void ESPIoTLog::verbose(const char* format, ...) {
    if (_log_level >= LOG_LEVEL_VERBOSE) {
        va_list args;
        va_start(args, format);
        logf(LOG_LEVEL_VERBOSE, format, args);
        va_end(args);
    }
}

void ESPIoTLog::log(log_level_t level, const char* format, ...) {
    if (_log_level >= level) {
        va_list args;
        va_start(args, format);
        logf(level, format, args);
        va_end(args);
    }
}

void ESPIoTLog::logf(log_level_t level, const char* format, va_list args) {
    if (!_initialized || _log_level < level) {
        return;
    }

    // Check log mask for runtime filtering
    uint8_t level_mask = 1 << (level - 1);
    if ((_log_mask & level_mask) == 0) {
        return;
    }

    // Output to Serial if enabled
    if (_serial_enabled) {
        char timestamp[16];
        snprintf(timestamp, sizeof(timestamp), "[%010lu]", millis());
        Serial.printf("%s [%s] ", timestamp, LOG_LEVEL_NAMES[level]);

#ifdef ESP32
        Serial.vprintf(format, args);
#elif defined(ESP8266)
        // ESP8266 doesn't have vprintf, so format manually
        char message[SERIAL_FORMAT_BUFFER_SIZE];
        vsnprintf(message, sizeof(message), format, args);
        Serial.print(message);
#endif
        Serial.println();
    }

    // Only send via network if listener is active
    if (!_listener_active) {
        return;
    }

    // Format message for network transmission
    vsnprintf(_log_buffer, _buffer_size - 1, format, args);
    _log_buffer[_buffer_size - 1] = '\0';

    formatLogMessage(level, _log_buffer);
}

void ESPIoTLog::logMetric(const char* name, int32_t value) {
    if (!_initialized || !_listener_active) return;
    if (!name || strlen(name) == 0) return;  // Invalid name

    // Create metric payload: name_length(1) + name + value(4)
    size_t name_len = strlen(name);
    if (name_len > 255) name_len = 255;

    uint8_t payload[260]; // 1 + 255 + 4
    payload[0] = (uint8_t)name_len;
    memcpy(payload + 1, name, name_len);
    memcpy(payload + 1 + name_len, &value, sizeof(value));

    sendLogMessage(LOG_TYPE_METRIC, payload, 1 + name_len + sizeof(value));
}

void ESPIoTLog::logMetric(const char* name, const char* value) {
    if (!_initialized || !_listener_active) return;
    if (!name || strlen(name) == 0) return;  // Invalid name
    if (!value) value = "";  // Allow empty value, but not NULL

    // Create string metric payload: name_length(1) + name + value_length(1) + value
    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    if (name_len > 255) name_len = 255;
    if (value_len > 255) value_len = 255;

    uint8_t payload[512]; // 1 + 255 + 1 + 255
    payload[0] = (uint8_t)name_len;
    memcpy(payload + 1, name, name_len);
    payload[1 + name_len] = (uint8_t)value_len;
    memcpy(payload + 2 + name_len, value, value_len);

    sendLogMessage(LOG_TYPE_METRIC, payload, 2 + name_len + value_len);
}

void ESPIoTLog::forceDiscovery() {
    _last_discovery = 0;  // Force immediate discovery
    _current_discovery_interval = DISCOVERY_INITIAL_INTERVAL;  // Reset backoff
}

void ESPIoTLog::sendTelemetry() {
    if (!_initialized || !_listener_active || _telemetry_flags == TELEMETRY_NONE) {
        return;
    }

    SystemTelemetry tel;
    collectTelemetry(tel);

    sendLogMessage(LOG_TYPE_TELEMETRY, (uint8_t*)&tel, sizeof(tel));
}

void ESPIoTLog::loop() {
    if (!_initialized) return;

    uint32_t now = millis();

    // Periodic discovery with exponential backoff
    if (now - _last_discovery >= _current_discovery_interval) {
        bool found = discoverListener();
        _last_discovery = now;

        if (found) {
            // Listener found - reset to minimum interval
            _current_discovery_interval = DISCOVERY_MIN_INTERVAL;
        } else {
            // No listener - exponential backoff up to max
            _current_discovery_interval = min(
                _current_discovery_interval * DISCOVERY_BACKOFF_MULTIPLIER,
                (uint32_t)DISCOVERY_MAX_INTERVAL
            );
            // Ensure we don't go below minimum
            if (_current_discovery_interval < DISCOVERY_MIN_INTERVAL) {
                _current_discovery_interval = DISCOVERY_MIN_INTERVAL;
            }
        }
    }

    // Periodic telemetry
    if (_listener_active && _telemetry_flags != TELEMETRY_NONE &&
        (now - _last_telemetry >= TELEMETRY_INTERVAL)) {
        sendTelemetry();
        _last_telemetry = now;
    }
}

bool ESPIoTLog::discoverListener() {
    // Query for the logging service
    int n = MDNS.queryService("esp-iot-log", "udp");

    bool found = (n > 0);

    if (found != _listener_active) {
        _listener_active = found;
        if (found) {
            info("Log listener discovered - logging enabled");
        } else {
            info("No log listeners found - network logging disabled");
        }
    }

    return found;
}

bool ESPIoTLog::sendLogMessage(log_type_t type, const uint8_t* data, size_t length) {
    if (!_initialized || !_listener_active) {
        _dropped_count++;
        return false;
    }

    // Prepare header
    LogHeader header;
    header.magic = LOG_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.device_id = _device_id;
    header.timestamp = millis();
    header.log_type = type;
    header.length = length;

    // Calculate total packet size
    size_t total_size = sizeof(header) + length + 2; // +2 for CRC16
    if (total_size > 1024) { // Reasonable UDP limit
        _dropped_count++;
        return false;
    }

    // Create packet
    uint8_t packet[1024];
    memcpy(packet, &header, sizeof(header));
    if (length > 0) {
        memcpy(packet + sizeof(header), data, length);
    }

    // Calculate and append checksum
    uint16_t checksum = calculateChecksum(packet, sizeof(header) + length);
    memcpy(packet + sizeof(header) + length, &checksum, sizeof(checksum));

    // Send UDP packet
    _udp.beginPacket(_multicast_addr, _multicast_port);
    size_t sent = _udp.write(packet, total_size);
    bool success = _udp.endPacket();

    if (success && sent == total_size) {
        _log_count++;
        return true;
    } else {
        _dropped_count++;
        return false;
    }
}

void ESPIoTLog::formatLogMessage(log_level_t level, const char* message) {
    if (!_listener_active) return;

    // Create text log payload: level(1) + message
    size_t msg_len = strlen(message);
    if (msg_len > DEFAULT_MAX_MESSAGE_SIZE - 1) {
        msg_len = DEFAULT_MAX_MESSAGE_SIZE - 1;
    }

    uint8_t payload[DEFAULT_MAX_MESSAGE_SIZE + 1];
    payload[0] = (uint8_t)level;
    memcpy(payload + 1, message, msg_len);

    sendLogMessage(LOG_TYPE_TEXT, payload, 1 + msg_len);
}

void ESPIoTLog::collectTelemetry(SystemTelemetry& tel) {
    memset(&tel, 0, sizeof(tel));

    if (_telemetry_flags & TELEMETRY_HEAP) {
        tel.heap_free = ESP.getFreeHeap();
#ifdef ESP32
        tel.heap_largest_block = ESP.getMaxAllocHeap();
        if (tel.heap_free > 0) {
            tel.heap_fragmentation = 100 - (tel.heap_largest_block * 100 / tel.heap_free);
        } else {
            tel.heap_fragmentation = 100;  // 100% fragmented if no free heap
        }
#elif defined(ESP8266)
        tel.heap_largest_block = ESP.getMaxFreeBlockSize();
        if (tel.heap_free > 0) {
            tel.heap_fragmentation = 100 - (tel.heap_largest_block * 100 / tel.heap_free);
        } else {
            tel.heap_fragmentation = 100;  // 100% fragmented if no free heap
        }
#endif
    }

    if (_telemetry_flags & TELEMETRY_WIFI) {
        tel.wifi_rssi = WiFi.RSSI();
        tel.wifi_status = WiFi.status();
        // Note: reconnect count would need custom tracking
        tel.wifi_reconnects = 0;
    }

    if (_telemetry_flags & TELEMETRY_TEMPERATURE) {
#ifdef ESP32
        tel.temperature = (int16_t)(temperatureRead() * 10); // Celsius * 10
#else
        tel.temperature = 0; // Not available on ESP8266
#endif
    }

    if (_telemetry_flags & TELEMETRY_RESET) {
#ifdef ESP32
        tel.reset_reason = esp_reset_reason();
#else
        tel.reset_reason = ESP.getResetInfoPtr()->reason;
#endif
    }

    if (_telemetry_flags & TELEMETRY_STACK) {
#ifdef ESP32
        tel.free_stack = uxTaskGetStackHighWaterMark(NULL);
#else
        tel.free_stack = 0; // Not easily available on ESP8266
#endif
    }

    tel.uptime = millis();

#ifdef ESP32
    // ESP-IDF Advanced telemetry collection
    if (_telemetry_flags & TELEMETRY_ADVANCED_HEAP) {
        collectAdvancedHeap(tel);
    }

    if (_telemetry_flags & TELEMETRY_TASK_INFO) {
        collectTaskInfo(tel);
    }

    if (_telemetry_flags & TELEMETRY_WIFI_ADVANCED) {
        collectAdvancedWiFi(tel);
    }

    if (_telemetry_flags & TELEMETRY_CHIP_INFO) {
        collectChipInfo(tel);
    }
#endif
}

#ifdef ESP32
void ESPIoTLog::collectAdvancedHeap(SystemTelemetry& tel) {
    // Per-capability heap information
    tel.advanced.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    tel.advanced.internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    // External PSRAM (if available)
    tel.advanced.external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    tel.advanced.external_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    // DMA-capable memory
    tel.advanced.dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
}

void ESPIoTLog::collectTaskInfo(SystemTelemetry& tel) {
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    tel.advanced.task_count = task_count < 255 ? (uint8_t)task_count : 255;

    // Find task with minimum stack remaining
    TaskStatus_t* task_array = (TaskStatus_t*)malloc(task_count * sizeof(TaskStatus_t));
    if (task_array) {
        UBaseType_t actual_count = uxTaskGetSystemState(task_array, task_count, NULL);

        uint32_t min_stack = UINT32_MAX;
        const char* critical_task_name = "unknown";

        for (UBaseType_t i = 0; i < actual_count; i++) {
            if (task_array[i].usStackHighWaterMark < min_stack) {
                min_stack = task_array[i].usStackHighWaterMark;
                critical_task_name = task_array[i].pcTaskName;
            }
        }

        tel.advanced.min_stack_remaining = min_stack < 65535 ? (uint16_t)min_stack : 65535;
        strncpy(tel.advanced.critical_task, critical_task_name, sizeof(tel.advanced.critical_task) - 1);
        tel.advanced.critical_task[sizeof(tel.advanced.critical_task) - 1] = '\0';

        free(task_array);
    } else {
        // malloc failed - set safe defaults
        tel.advanced.min_stack_remaining = 0;
        strncpy(tel.advanced.critical_task, "malloc_fail", sizeof(tel.advanced.critical_task) - 1);
        tel.advanced.critical_task[sizeof(tel.advanced.critical_task) - 1] = '\0';
    }
}

void ESPIoTLog::collectAdvancedWiFi(SystemTelemetry& tel) {
    if (WiFi.isConnected()) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            tel.wifi_advanced.channel = ap_info.primary;
        }

        // Get WiFi bandwidth
        wifi_bandwidth_t bw;
        if (esp_wifi_get_bandwidth(WIFI_IF_STA, &bw) == ESP_OK) {
            tel.wifi_advanced.bandwidth = (bw == WIFI_BW_HT40) ? 40 : 20;
        }

        // Get TX power
        int8_t power;
        if (esp_wifi_get_max_tx_power(&power) == ESP_OK) {
            tel.wifi_advanced.tx_power = power;
        }

        // Get country code
        wifi_country_t country;
        if (esp_wifi_get_country(&country) == ESP_OK) {
            memcpy(tel.wifi_advanced.country_code, country.cc, sizeof(country.cc));
            tel.wifi_advanced.country_code[sizeof(country.cc)] = '\0';
        }
    }
}

void ESPIoTLog::collectChipInfo(SystemTelemetry& tel) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    tel.chip_info.chip_model = chip_info.model;
    tel.chip_info.chip_cores = chip_info.cores;
    tel.chip_info.chip_revision = chip_info.revision;

    // Flash information
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        tel.chip_info.flash_size = flash_size;
    }

    tel.chip_info.cpu_freq_mhz = ESP.getCpuFreqMHz();

    // Security features (if available)
#ifdef CONFIG_SECURE_BOOT_ENABLED
    tel.chip_info.secure_boot = esp_secure_boot_enabled();
#else
    tel.chip_info.secure_boot = false;
#endif

#ifdef CONFIG_FLASH_ENCRYPTION_ENABLED
    tel.chip_info.flash_encryption = esp_flash_encryption_enabled();
#else
    tel.chip_info.flash_encryption = false;
#endif
}
#endif

void ESPIoTLog::logStartupInfo() {
    if (!_initialized || !_listener_active) return;

#ifdef ESP32
    if (_telemetry_flags & TELEMETRY_CHIP_INFO) {
        esp_chip_info_t chip_info;
        esp_chip_info(&chip_info);

        const char* chip_name;
        switch(chip_info.model) {
            case CHIP_ESP32: chip_name = "ESP32"; break;
            case CHIP_ESP32S2: chip_name = "ESP32-S2"; break;
            case CHIP_ESP32S3: chip_name = "ESP32-S3"; break;
            case CHIP_ESP32C3: chip_name = "ESP32-C3"; break;
            case CHIP_ESP32C2: chip_name = "ESP32-C2"; break;
            case CHIP_ESP32C6: chip_name = "ESP32-C6"; break;
            case CHIP_ESP32H2: chip_name = "ESP32-H2"; break;
            default: chip_name = "Unknown ESP32"; break;
        }

        info("=== ESP32 Hardware Information ===");
        info("Chip: %s (Rev %d.%d)", chip_name, chip_info.revision / 100, chip_info.revision % 100);
        info("Cores: %d", chip_info.cores);
        info("Features: WiFi%s%s",
             (chip_info.features & CHIP_FEATURE_BT) ? "+BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "+BLE" : "");

        uint32_t flash_size;
        if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
            info("Flash: %lu MB", flash_size / (1024 * 1024));
        }

        info("CPU Frequency: %lu MHz", ESP.getCpuFreqMHz());

        // PSRAM info
        if (chip_info.features & CHIP_FEATURE_EMB_PSRAM) {
            info("PSRAM: %lu KB", ESP.getPsramSize() / 1024);
        }

        info("==================================");
    }
#endif
}

uint64_t ESPIoTLog::getDeviceId() {
#ifdef ESP32
    return ESP.getEfuseMac();
#else
    return ESP.getChipId(); // 32-bit on ESP8266, extend to 64-bit
#endif
}

uint16_t ESPIoTLog::calculateChecksum(const uint8_t* data, size_t length) {
    // Simple CRC16-CCITT
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

void ESPIoTLog::enableCrashLogging(bool enable) {
    _crash_logging_enabled = enable;

    if (enable) {
        ESPCrashHandler::install();
        info("Crash logging enabled");
    } else {
        ESPCrashHandler::uninstall();
        info("Crash logging disabled");
    }
}

void ESPIoTLog::checkAndLogCrashes() {
    if (!_initialized || !_crash_logging_enabled) return;

    if (ESPCrashHandler::hasCrashData()) {
        CrashData crash;
        if (ESPCrashHandler::getCrashData(crash)) {
            error("Previous crash detected: type=%d, reason=%d, heap=%dKB, uptime=%lums, function=%s",
                  crash.crash_type, crash.reset_reason, crash.heap_free,
                  crash.timestamp, crash.last_function[0] ? crash.last_function : "unknown");

            // Send crash data if listener is active
            if (_listener_active) {
                sendLogMessage(LOG_TYPE_EXCEPTION, (uint8_t*)&crash, sizeof(crash));
            }

            uint32_t total_crashes = ESPCrashHandler::getCrashCount();
            if (total_crashes > 1) {
                warn("Total crash count: %lu", total_crashes);
            }
        }

        // Clear crash data after logging
        ESPCrashHandler::clearCrashData();
    }
}

void ESPIoTLog::logCrash(const char* reason) {
    if (!_initialized) return;

    error("Manual crash report: %s", reason);

    if (_crash_logging_enabled) {
        ESPCrashHandler::recordCrash(CRASH_TYPE_UNKNOWN, reason);
    }
}