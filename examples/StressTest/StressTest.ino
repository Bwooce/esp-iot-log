/**
 * ESP IoT Log - Stress Test Example
 * Copyright (c) 2026 Bruce Fitzsimons
 *
 * Stress test for the ESP IoT Log library to verify:
 * - High message throughput handling
 * - Memory management under load
 * - Network stability with rapid logging
 * - Buffer overflow protection
 * - Performance metrics
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

// Test parameters
const int MESSAGES_PER_SECOND = 10;
const int BURST_SIZE = 5;
const int TEST_DURATION_MINUTES = 5;

// Performance tracking
unsigned long testStartTime = 0;
unsigned long totalMessages = 0;
unsigned long droppedAtStart = 0;
unsigned long lastMemoryCheck = 0;
unsigned long minFreeHeap = UINT32_MAX;

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    Serial.println("\n=== ESP IoT Log - Stress Test ===");

    // Connect to WiFi
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());

    // Initialize with VERBOSE logging to generate maximum traffic
    if (!iotlog.begin("StressTest", LOG_LEVEL_VERBOSE)) {
        Serial.println("Failed to initialize iotlog!");
        return;
    }

    // Enable basic telemetry
    iotlog.enableSystemTelemetry(TELEMETRY_HEAP | TELEMETRY_WIFI);

    // Configure for aggressive discovery
    iotlog.setDiscoveryInterval(5000);  // Check every 5 seconds

    // Record baseline metrics
    droppedAtStart = iotlog.getDroppedCount();
    minFreeHeap = ESP.getFreeHeap();
    testStartTime = millis();

    iotlog.info("=== STRESS TEST STARTING ===");
    iotlog.info("Target rate: %d messages/second", MESSAGES_PER_SECOND);
    iotlog.info("Burst size: %d messages", BURST_SIZE);
    iotlog.info("Test duration: %d minutes", TEST_DURATION_MINUTES);
    iotlog.info("Initial free heap: %d bytes", ESP.getFreeHeap());

    Serial.println("\nStarting stress test...");
    Serial.printf("Will generate %d messages/second for %d minutes\n",
                  MESSAGES_PER_SECOND, TEST_DURATION_MINUTES);
    Serial.println("Start the Python receiver to see network performance");
}

void loop() {
    // Essential library maintenance
    iotlog.loop();

    // Check if test duration exceeded
    unsigned long elapsedSeconds = (millis() - testStartTime) / 1000;
    if (elapsedSeconds >= TEST_DURATION_MINUTES * 60) {
        finishTest();
        return;
    }

    // Generate message bursts
    generateMessageBurst();

    // Memory monitoring
    monitorSystemHealth();

    // Delay to achieve target rate
    delay(1000 / MESSAGES_PER_SECOND);
}

void generateMessageBurst() {
    static int burstCounter = 0;

    for (int i = 0; i < BURST_SIZE; i++) {
        totalMessages++;
        burstCounter++;

        switch (burstCounter % 6) {
            case 0:
                iotlog.verbose("Verbose message #%lu - detailed system state", totalMessages);
                break;
            case 1:
                iotlog.debug("Debug burst %d/%d - heap: %d", i+1, BURST_SIZE, ESP.getFreeHeap());
                break;
            case 2:
                iotlog.info("Info message %lu - uptime: %lus", totalMessages, millis()/1000);
                break;
            case 3:
                iotlog.warn("Warning #%lu - high load test in progress", totalMessages);
                break;
            case 4:
                if (totalMessages % 100 == 0) {
                    iotlog.error("Error simulation #%lu - stress test marker", totalMessages);
                }
                break;
            case 5:
                // Send custom metrics during stress test
                iotlog.logMetric("stress_counter", (int32_t)totalMessages);
                iotlog.logMetric("free_heap", (int32_t)ESP.getFreeHeap());
                break;
        }
    }
}

void monitorSystemHealth() {
    if (millis() - lastMemoryCheck >= 10000) { // Every 10 seconds
        lastMemoryCheck = millis();

        uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < minFreeHeap) {
            minFreeHeap = freeHeap;
        }

        // Log statistics
        unsigned long elapsedSeconds = (millis() - testStartTime) / 1000;
        float rate = elapsedSeconds > 0 ? (float)totalMessages / elapsedSeconds : 0;

        iotlog.info("=== STRESS TEST STATUS ===");
        iotlog.info("Elapsed: %lus, Messages: %lu, Rate: %.1f/s",
                    elapsedSeconds, totalMessages, rate);
        iotlog.info("Free heap: %d bytes (min: %d)", freeHeap, minFreeHeap);
        iotlog.info("Network logs: %lu sent, %lu dropped",
                    iotlog.getLogCount(), iotlog.getDroppedCount() - droppedAtStart);
        iotlog.info("Listener: %s, WiFi: %d dBm",
                    iotlog.isListenerActive() ? "ACTIVE" : "NONE", WiFi.RSSI());
        iotlog.info("========================");

        // Print to serial as well
        Serial.printf("[STRESS] %lus: %lu msgs (%.1f/s), heap: %d/%d, network: %s\n",
                     elapsedSeconds, totalMessages, rate, freeHeap, minFreeHeap,
                     iotlog.isListenerActive() ? "ON" : "OFF");

        // Memory leak detection
        if (freeHeap < minFreeHeap * 0.8) {
            iotlog.error("MEMORY WARNING: Heap dropped to %d bytes!", freeHeap);
        }

        // Check for excessive drops
        unsigned long dropsInTest = iotlog.getDroppedCount() - droppedAtStart;
        if (dropsInTest > totalMessages * 0.1) { // More than 10% drop rate
            iotlog.warn("HIGH DROP RATE: %lu drops out of %lu messages (%.1f%%)",
                       dropsInTest, totalMessages, (float)dropsInTest/totalMessages*100);
        }
    }
}

void finishTest() {
    unsigned long elapsedSeconds = (millis() - testStartTime) / 1000;
    unsigned long dropsInTest = iotlog.getDroppedCount() - droppedAtStart;
    float avgRate = elapsedSeconds > 0 ? (float)totalMessages / elapsedSeconds : 0;
    float dropRate = totalMessages > 0 ? (float)dropsInTest / totalMessages * 100 : 0;

    iotlog.info("=== STRESS TEST COMPLETE ===");
    iotlog.info("Duration: %lu seconds", elapsedSeconds);
    iotlog.info("Total messages: %lu", totalMessages);
    iotlog.info("Average rate: %.2f messages/second", avgRate);
    iotlog.info("Network sent: %lu", iotlog.getLogCount());
    iotlog.info("Network dropped: %lu (%.1f%%)", dropsInTest, dropRate);
    iotlog.info("Min free heap: %d bytes", minFreeHeap);
    iotlog.info("Final free heap: %d bytes", ESP.getFreeHeap());
    iotlog.info("Heap efficiency: %.1f%% retained",
                (float)ESP.getFreeHeap() / minFreeHeap * 100);
    iotlog.info("============================");

    // Print summary to Serial
    Serial.println("\n=== STRESS TEST RESULTS ===");
    Serial.printf("Duration: %lu seconds\n", elapsedSeconds);
    Serial.printf("Messages: %lu (%.2f/s average)\n", totalMessages, avgRate);
    Serial.printf("Network: %lu sent, %lu dropped (%.1f%% drop rate)\n",
                  iotlog.getLogCount(), dropsInTest, dropRate);
    Serial.printf("Memory: %d bytes min, %d bytes final\n",
                  minFreeHeap, ESP.getFreeHeap());

    if (dropRate < 5.0) {
        Serial.println("RESULT: ✓ PASS - Low drop rate");
    } else if (dropRate < 15.0) {
        Serial.println("RESULT: ⚠ MARGINAL - Moderate drop rate");
    } else {
        Serial.println("RESULT: ✗ FAIL - High drop rate");
    }

    Serial.println("Test complete. Reset to run again.");

    // Infinite loop
    while (true) {
        iotlog.loop();
        delay(1000);
    }
}