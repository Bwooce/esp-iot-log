# ESP IoT Log

A lightweight, efficient logging library for ESP32 and ESP8266 that uses mDNS discovery to detect active listeners before sending multicast UDP logs. This ensures **zero network overhead when no monitoring is active** while providing rich telemetry and debugging capabilities when needed.

## Features

### 🚀 Smart Discovery
- **mDNS Service Discovery**: Automatically detects listening applications
- **Zero Overhead**: No network traffic when no listeners are present
- **Automatic Activation**: Starts logging when Python receiver (or compatible listener) is detected

### 📊 ESP-Specific Telemetry
- **System Health**: Heap, stack, flash usage, fragmentation analysis
- **Connectivity**: WiFi RSSI, connection state, reconnect counts
- **Reliability**: Reset reasons, watchdog events, uptime tracking
- **Performance**: CPU temperature, load estimation, memory efficiency
- **Power**: Sleep cycle analysis, current consumption patterns

### 🔧 Crash Logging
- **Exception Handling**: Captures crashes, panics, and watchdog timeouts
- **RTC Persistence**: Crash data survives warm resets via RTC memory
- **Stack Traces**: Basic backtrace and register dump capabilities
- **Recovery Tracking**: Count and categorize crash types over time

### 📡 Efficient Network Protocol
- **Binary Format**: Minimal bandwidth usage with structured data
- **Multicast UDP**: Efficient 1:N delivery to multiple monitoring tools
- **CRC16 Checksums**: Data integrity verification
- **Batched Transmission**: Reduces network overhead and improves performance

### 🛠️ Developer Friendly
- **Arduino Library**: Standard installation via Library Manager
- **Cross-Platform Receiver**: Python script works on Windows, macOS, Linux
- **Rich API**: Simple logging calls with printf-style formatting
- **Real-time Monitoring**: Live log streaming with color-coded output
- **JSON Export**: Machine-readable output for integration with other tools

## Quick Start

### 1. Installation

**Arduino IDE:**
```
Library Manager → Search "ESP IoT Log" → Install
```

**PlatformIO:**
```
platformio lib install "ESP IoT Log"
```

**Python Receiver:**
```bash
pip install -r python/requirements.txt
```

### 2. Basic Arduino Code

```cpp
#include <WiFi.h>
#include <ESPiotlog.h>

void setup() {
    Serial.begin(115200);

    // Connect to WiFi
    WiFi.begin("your-ssid", "your-password");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    // Initialize logging
    iotlog.begin("MyDevice", LOG_LEVEL_INFO);

    // Enable system telemetry
    iotlog.enableSystemTelemetry(TELEMETRY_HEAP | TELEMETRY_WIFI);

    // Enable crash logging
    iotlog.enableCrashLogging(true);

    iotlog.info("Device started successfully");
}

void loop() {
    iotlog.loop();  // Essential: handles discovery and network operations

    // Your application logs
    iotlog.debug("Sensor reading: %d", analogRead(A0));

    delay(1000);
}
```

### 3. Start Python Receiver

```bash
# Start listening for logs
python python/esp_iot_log_receiver.py

# With options
python python/esp_iot_log_receiver.py --output logs.txt --json
```

### 4. Watch the Magic ✨

1. **Start receiver first** → Advertises mDNS service
2. **Power on ESP device** → Discovers receiver via mDNS
3. **Automatic logging** → ESP starts sending logs to receiver
4. **Stop receiver** → ESP automatically stops network logging (saves bandwidth)

## API Reference

### Initialization

```cpp
// Basic initialization
iotlog.begin("DeviceName", LOG_LEVEL_INFO);

// Configure multicast settings
iotlog.setMulticastAddress("239.255.1.100", 4210);

// Set discovery frequency
iotlog.setDiscoveryInterval(10000);  // Check every 10 seconds

// Runtime log filtering (inspired by arcao/Syslog)
iotlog.setLogMask(LOG_MASK_ERROR | LOG_MASK_WARN);  // Only errors and warnings
iotlog.setLogMask(LOG_MASK_ALL);                    // All levels (default)
```

### Logging Methods

```cpp
// Standard log levels
iotlog.error("Critical error: %s", errorMsg);
iotlog.warn("Warning: sensor %d offline", sensorId);
iotlog.info("System started, free heap: %d", ESP.getFreeHeap());
iotlog.debug("Debug info: loop iteration %d", counter);
iotlog.verbose("Detailed trace: function %s called", __func__);

// Direct level control
iotlog.log(LOG_LEVEL_ERROR, "Custom level message");

// Convenience macros (if preferred)
LOG_ERROR("Something went wrong!");
LOG_INFO("Status update");
```

### System Telemetry

```cpp
// Enable telemetry categories
iotlog.enableSystemTelemetry(
    TELEMETRY_HEAP |        // Memory usage, fragmentation
    TELEMETRY_WIFI |        // Signal strength, connection state
    TELEMETRY_TEMPERATURE | // Core temperature (ESP32)
    TELEMETRY_STACK |       // Stack high water marks
    TELEMETRY_RESET |       // Reset reasons, boot counts
    TELEMETRY_ALL          // Everything
);

// Manual telemetry transmission
iotlog.sendTelemetry();
```

### Custom Metrics

```cpp
// Numeric metrics
iotlog.logMetric("cpu_usage", cpuPercent);
iotlog.logMetric("sensor_temp", temperatureReading);
iotlog.logMetric("error_count", errorCounter);

// String metrics
iotlog.logMetric("device_state", "operational");
iotlog.logMetric("last_error", "timeout");
```

### Crash Logging

```cpp
// Enable crash detection and logging
iotlog.enableCrashLogging(true);

// Manual crash recording
iotlog.logCrash("Custom error condition");

// Check for previous crashes (called automatically in begin())
iotlog.checkAndLogCrashes();
```

### Status and Statistics

```cpp
// Check if network logging is active
if (iotlog.isListenerActive()) {
    Serial.println("Logs are being sent to network");
}

// Get statistics
uint32_t sent = iotlog.getLogCount();
uint32_t dropped = iotlog.getDroppedCount();

// Force operations
iotlog.forceDiscovery();    // Check for listeners now
iotlog.sendTelemetry();     // Send telemetry now
```

## Python Receiver

### Command Line Options

```bash
python esp_iot_log_receiver.py [options]

Options:
  --ip IP          Multicast IP (default: 239.255.1.100)
  --port PORT      UDP port (default: 4210)
  --service NAME   mDNS service name
  --output FILE    Save logs to file
  --json           Output in JSON format
  --no-color       Disable colored output
```

### Example Output

```
14:23:45.123 ABC123DE [00:01:23.456] [   INFO] Device started successfully
14:23:46.234 ABC123DE [00:01:24.567] [  DEBUG] Sensor reading: 512
14:23:47.345 ABC123DE [00:01:25.678] [TELEMETRY] Heap: 245760B (frag: 12%), WiFi: -45dBm, Temp: 43.2°C
14:23:48.456 ABC123DE [00:01:26.789] [ METRIC] sensor_temp = 23.5
```

## Protocol Specification

### Message Structure

```
Header (19 bytes):
  Magic Number: 0xE510 (2 bytes)
  Protocol Version: 1 (1 byte)
  Device ID: MAC address (8 bytes)
  Timestamp: millis() since boot (4 bytes)
  Log Type: Message type (1 byte)
  Length: Payload length (2 bytes)

Payload: Variable length data

Checksum: CRC16-CCITT (2 bytes)
```

### Log Types

| Type | Value | Description |
|------|-------|-------------|
| TEXT | 0x01 | Standard log messages with level |
| TELEMETRY | 0x02 | System telemetry data |
| EXCEPTION | 0x03 | Crash/exception reports |
| METRIC | 0x04 | Custom application metrics |

### mDNS Service

- **Service Type**: `_esp-iot-log._udp.local.`
- **Properties**: `multicast=239.255.1.100, version=1.0`
- **Discovery**: ESP devices query for this service to detect listeners

## Troubleshooting

### Common Issues

**No logs appearing in receiver:**
1. Check WiFi connectivity on ESP device
2. Ensure receiver is started before ESP device
3. Verify multicast IP/port configuration
4. Check firewall settings (allow UDP multicast)

**High message drop rate:**
1. Reduce logging frequency
2. Lower log level (INFO instead of VERBOSE)
3. Disable unnecessary telemetry categories
4. Check network congestion

**Crash logging not working:**
1. Call `iotlog.enableCrashLogging(true)` in setup()
2. Verify RTC memory is available
3. Check that crashes are severe enough to trigger handlers

**mDNS discovery fails:**
1. Ensure both devices on same network segment
2. Check for mDNS/Bonjour service availability
3. Try manual discovery with `iotlog.forceDiscovery()`

### Debug Information

```cpp
// Enable verbose logging to see library internals
iotlog.setLogLevel(LOG_LEVEL_VERBOSE);

// Check discovery status
Serial.printf("Listener active: %s\n",
              iotlog.isListenerActive() ? "YES" : "NO");

// Monitor statistics
Serial.printf("Sent: %lu, Dropped: %lu\n",
              iotlog.getLogCount(), iotlog.getDroppedCount());
```

## Performance Characteristics

### Memory Usage
- **Library overhead**: ~8KB flash, ~2KB RAM
- **Log buffer**: 512 bytes (configurable)
- **Zero allocation**: When no listeners detected

### Network Performance
- **Message rate**: 50-100 messages/second typical
- **Bandwidth**: ~100 bytes per text log message
- **Latency**: <10ms from log call to network transmission

### Power Consumption
- **Idle**: No additional current draw when listeners inactive
- **Active**: ~5mA additional during network transmission
- **Sleep**: Compatible with deep sleep modes

## ESP32 vs ESP8266 Differences

| Feature | ESP32 | ESP8266 |
|---------|-------|---------|
| Core Temperature | ✓ Available | ✗ Not available |
| Heap Fragmentation | ✓ Detailed stats | ✗ Basic only |
| Stack Monitoring | ✓ FreeRTOS support | ✗ Limited |
| Crash Handling | ✓ Full exception hooks | ✓ Basic reset info |
| PSRAM Support | ✓ If available | ✗ Not supported |
| Performance | Higher throughput | Lower but adequate |

## Architecture

### Design Principles

1. **Zero Overhead When Idle**: No network activity if no listeners
2. **Automatic Discovery**: Uses mDNS to find monitoring applications
3. **Robust Protocol**: Binary format with checksums for reliability
4. **ESP-Optimized**: Designed specifically for ESP32/ESP8266 capabilities
5. **Non-Blocking**: All operations are asynchronous and ISR-safe

### Library Components

- **ESPiotlog**: Main logging interface and network management
- **ESPCrashHandler**: Exception handling and RTC memory persistence
- **Binary Protocol**: Efficient serialization and checksumming
- **mDNS Integration**: Service discovery and advertising
- **Python Receiver**: Cross-platform log collection and display

## Examples

### Basic Logging
Simple logging setup with automatic discovery.

### Advanced Features
Comprehensive example showing telemetry, crash logging, and custom metrics.

### Stress Test
Performance testing with high message rates and memory monitoring.

## License

MIT License - Copyright (c) 2026 Bruce Fitzsimons <bruce@fitzsimons.org>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

See [LICENSE](LICENSE) file for full details.

## Contributing

Issues and pull requests welcome! Please see CONTRIBUTING.md for guidelines.

## Roadmap

- **v1.1**: Web-based log viewer dashboard
- **v1.2**: Log compression and encryption options
- **v1.3**: Fleet management and multi-device aggregation
- **v2.0**: Integration with popular IoT platforms