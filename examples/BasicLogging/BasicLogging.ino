/**
 * ESP IoT Log - Basic Logging Example
 * Copyright (c) 2026 Bruce Fitzsimons
 *
 * Simple example showing how to use the ESP IoT Log library for
 * multicast logging with mDNS discovery.
 *
 * This example demonstrates:
 * - Basic library initialization
 * - Different log levels (ERROR, WARN, INFO, DEBUG, VERBOSE)
 * - Automatic mDNS discovery
 * - Zero overhead when no listeners are present
 */

#ifdef ESP32
  #include <WiFi.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
#endif
#include <ESPIoTLog.h>

// WiFi credentials
const char* ssid = "your-wifi-ssid";
const char* password = "your-wifi-password";

// Timing variables
unsigned long lastLogTime = 0;
unsigned long logInterval = 5000;  // Log every 5 seconds
int counter = 0;

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    Serial.println("\n=== ESP IoT Log - Basic Example ===");

    // Connect to WiFi
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Initialize IoT logging
    // Use device name "BasicDemo" and INFO log level
    if (!iotlog.begin("BasicDemo", LOG_LEVEL_INFO)) {
        Serial.println("Failed to initialize iotlog!");
        return;
    }

    // The library will automatically:
    // 1. Check for mDNS listeners
    // 2. Start logging to network if found
    // 3. Always log to Serial regardless

    iotlog.info("Basic logging example started");
    iotlog.info("Device: %s", WiFi.macAddress().c_str());
    iotlog.info("Free heap: %d bytes", ESP.getFreeHeap());

    Serial.println("\nTo see network logs:");
    Serial.println("1. Install Python dependencies: pip install -r python/requirements.txt");
    Serial.println("2. Run receiver: python python/esp_iot_log_receiver.py");
    Serial.println("3. Watch for automatic discovery and log delivery");
}

void loop() {
    // Let the logging library check for listeners and handle discovery
    iotlog.loop();

    // Generate some example logs
    if (millis() - lastLogTime >= logInterval) {
        lastLogTime = millis();
        counter++;

        // Demonstrate different log levels
        switch (counter % 5) {
            case 0:
                iotlog.info("Periodic status update #%d", counter);
                break;
            case 1:
                iotlog.debug("Debug message - heap: %d bytes", ESP.getFreeHeap());
                break;
            case 2:
                iotlog.warn("Warning: This is just a test warning");
                break;
            case 3:
                iotlog.verbose("Verbose details: counter=%d, uptime=%lums", counter, millis());
                break;
            case 4:
                // Simulate an error condition
                if (counter > 20) {
                    iotlog.error("Simulated error after %d iterations", counter);
                }
                break;
        }

        // Show listener status
        if (iotlog.isListenerActive()) {
            iotlog.debug("Network logging active - sent %lu logs", iotlog.getLogCount());
        } else {
            // This will only show locally (not sent to network)
            Serial.printf("[INFO] No network listeners - logs staying local\n");
        }
    }

    delay(100);  // Small delay to prevent watchdog issues
}