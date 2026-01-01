/**
 * ESP IoT Log - Advanced Features Example
 * Copyright (c) 2026 Bruce Fitzsimons
 *
 * Comprehensive example demonstrating all features of the ESP IoT Log library:
 * - System telemetry collection and transmission
 * - Crash detection and logging
 * - Custom metrics
 * - Configuration options
 * - Statistics and monitoring
 */

#ifdef ESP32
  #include <WiFi.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
#endif
#include <ESPiotlog.h>

// WiFi credentials
const char* ssid = "your-wifi-ssid";
const char* password = "your-wifi-password";

// Timing variables
unsigned long lastMetricTime = 0;
unsigned long lastStatusTime = 0;
int sensorReading = 0;
float cpuUsage = 0.0;

// Simulate application state
bool applicationHealthy = true;
int errorCount = 0;

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    Serial.println("\n=== ESP IoT Log - Advanced Features Example ===");

    // Connect to WiFi
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Initialize IoT logging with DEBUG level
    if (!iotlog.begin("AdvancedDemo", LOG_LEVEL_DEBUG)) {
        Serial.println("Failed to initialize iotlog!");
        return;
    }

    // Enable crash logging (survives resets)
    iotlog.enableCrashLogging(true);

    // Enable comprehensive system telemetry
    iotlog.enableSystemTelemetry(
        TELEMETRY_HEAP |
        TELEMETRY_WIFI |
        TELEMETRY_TEMPERATURE |
        TELEMETRY_STACK |
        TELEMETRY_RESET
    );

    // Configure discovery and multicast settings
    iotlog.setDiscoveryInterval(15000);  // Check for listeners every 15 seconds

    iotlog.info("Advanced features demo started");
    iotlog.info("Device: %s", WiFi.macAddress().c_str());
    iotlog.info("Chip model: %s", ESP.getChipModel());
    iotlog.info("SDK version: %s", ESP.getSdkVersion());

    // Log initial system state
    logSystemInfo();

    Serial.println("\nAvailable commands (send via Serial):");
    Serial.println("  'crash' - Simulate a crash");
    Serial.println("  'error' - Simulate application error");
    Serial.println("  'reset' - Force telemetry transmission");
    Serial.println("  'stats' - Show logging statistics");
    Serial.println("  'force' - Force listener discovery");
}

void loop() {
    // Essential: Let library handle discovery and network operations
    iotlog.loop();

    // Handle serial commands
    handleSerialCommands();

    // Periodic custom metrics (every 10 seconds)
    if (millis() - lastMetricTime >= 10000) {
        lastMetricTime = millis();
        sendCustomMetrics();
    }

    // Periodic status logging (every 30 seconds)
    if (millis() - lastStatusTime >= 30000) {
        lastStatusTime = millis();
        logApplicationStatus();
    }

    // Simulate some application work
    simulateApplicationWork();

    delay(100);
}

void logSystemInfo() {
    iotlog.info("=== System Information ===");
    iotlog.info("Free heap: %d bytes", ESP.getFreeHeap());
    iotlog.info("Chip revision: %d", ESP.getChipRevision());
    iotlog.info("Flash size: %d bytes", ESP.getFlashChipSize());
    iotlog.info("Flash speed: %d Hz", ESP.getFlashChipSpeed());

#ifdef ESP32
    iotlog.info("Max alloc heap: %d bytes", ESP.getMaxAllocHeap());
    iotlog.info("PSRAM size: %d bytes", ESP.getPsramSize());
    iotlog.info("CPU frequency: %d MHz", ESP.getCpuFreqMHz());
#endif

    iotlog.info("WiFi RSSI: %d dBm", WiFi.RSSI());
    iotlog.info("============================");
}

void sendCustomMetrics() {
    // Simulate sensor readings
    sensorReading = random(20, 30);  // Temperature sensor
    int humidity = random(40, 60);   // Humidity sensor
    int lightLevel = random(0, 1023); // Light sensor

    // Send various metric types
    iotlog.logMetric("temperature", sensorReading);
    iotlog.logMetric("humidity", humidity);
    iotlog.logMetric("light_level", lightLevel);

    // Calculate and log CPU usage (simplified simulation)
    cpuUsage = random(5, 25) / 100.0;
    iotlog.logMetric("cpu_usage", (int32_t)(cpuUsage * 100)); // Send as percentage * 100

    // Application metrics
    iotlog.logMetric("error_count", errorCount);
    iotlog.logMetric("app_healthy", applicationHealthy ? "true" : "false");

    iotlog.debug("Custom metrics sent - temp: %d°C, humidity: %d%%, light: %d",
                 sensorReading, humidity, lightLevel);
}

void logApplicationStatus() {
    iotlog.info("=== Application Status ===");
    iotlog.info("Uptime: %lu seconds", millis() / 1000);
    iotlog.info("Loop iterations: ~%lu", millis() / 100);
    iotlog.info("Error count: %d", errorCount);
    iotlog.info("Health status: %s", applicationHealthy ? "GOOD" : "DEGRADED");

    // Network logging statistics
    iotlog.info("Logs sent: %lu", iotlog.getLogCount());
    iotlog.info("Logs dropped: %lu", iotlog.getDroppedCount());
    iotlog.info("Listener active: %s", iotlog.isListenerActive() ? "YES" : "NO");

    iotlog.info("==========================");
}

void simulateApplicationWork() {
    static unsigned long lastWork = 0;
    static int workCounter = 0;

    if (millis() - lastWork >= 2000) { // Every 2 seconds
        lastWork = millis();
        workCounter++;

        // Simulate occasional errors
        if (random(100) < 5) { // 5% chance
            errorCount++;
            iotlog.warn("Simulated application warning #%d", errorCount);

            if (errorCount > 10) {
                applicationHealthy = false;
                iotlog.error("Application health degraded after %d errors", errorCount);
            }
        }

        // Simulate verbose debug information
        if (workCounter % 5 == 0) {
#ifdef ESP32
            iotlog.verbose("Work cycle #%d completed, free stack: %d bytes",
                          workCounter, uxTaskGetStackHighWaterMark(NULL));
#elif defined(ESP8266)
            iotlog.verbose("Work cycle #%d completed, heap: %d bytes",
                          workCounter, ESP.getFreeHeap());
#endif
        }
    }
}

void handleSerialCommands() {
    if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        command.toLowerCase();

        if (command == "crash") {
            iotlog.warn("Simulating crash in 3 seconds...");
            delay(3000);
            iotlog.error("Triggering intentional crash for testing");
            iotlog.logCrash("Manual crash test");

            // Force a crash (DO NOT USE IN PRODUCTION!)
            int* p = nullptr;
            *p = 42;  // This will cause a crash

        } else if (command == "error") {
            errorCount += 5;
            iotlog.error("Simulated critical error - count now: %d", errorCount);
            if (errorCount > 20) {
                applicationHealthy = false;
            }

        } else if (command == "reset") {
            iotlog.info("Forcing telemetry transmission...");
            iotlog.sendTelemetry();

        } else if (command == "stats") {
            logApplicationStatus();

        } else if (command == "force") {
            iotlog.info("Forcing listener discovery...");
            iotlog.forceDiscovery();

        } else if (command.length() > 0) {
            iotlog.warn("Unknown command: %s", command.c_str());
            Serial.println("Available commands: crash, error, reset, stats, force");
        }
    }
}