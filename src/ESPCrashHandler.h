/**
 * ESP Crash Handler - Capture and log crashes/exceptions
 * Copyright (c) 2026 Bruce Fitzsimons
 */

#ifndef ESP_CRASH_HANDLER_H
#define ESP_CRASH_HANDLER_H

#include <Arduino.h>

#ifdef ESP32
  #include <esp_system.h>
  #include <esp_err.h>
  #include <esp_core_dump.h>
#elif defined(ESP8266)
  #include <user_interface.h>
  extern "C" {
    #include <cont.h>
    void custom_crash_callback(struct rst_info* rst_info, uint32_t stack, uint32_t stack_end);
  }
#endif

// Crash data stored in RTC memory (survives warm resets)
struct __attribute__((packed)) CrashData {
    uint32_t magic;           // 0xDEADBEEF
    uint32_t timestamp;       // millis() at crash
    uint8_t crash_type;       // Exception type
    uint8_t reset_reason;     // Hardware reset reason
    uint16_t heap_free;       // Free heap at crash
    uint32_t stack_ptr;       // Stack pointer at crash
    uint32_t pc;              // Program counter
    uint32_t backtrace[8];    // Simplified backtrace
    char last_function[32];   // Last function name (if available)
    uint8_t checksum;         // Simple checksum for validation
};

// Crash types
typedef enum {
    CRASH_TYPE_UNKNOWN = 0,
    CRASH_TYPE_PANIC = 1,
    CRASH_TYPE_EXCEPTION = 2,
    CRASH_TYPE_WATCHDOG = 3,
    CRASH_TYPE_BROWNOUT = 4,
    CRASH_TYPE_STACK_OVERFLOW = 5,
    CRASH_TYPE_HEAP_CORRUPT = 6
} crash_type_t;

class ESPCrashHandler {
public:
    static void install();
    static void uninstall();

    // Check for crash data from previous boot
    static bool hasCrashData();
    static bool getCrashData(CrashData& data);
    static void clearCrashData();

    // Manual crash recording (for custom error conditions)
    // reset_reason: pass -1 to read from ESP.getResetInfoPtr(), or explicit value from crash callback
    static void recordCrash(crash_type_t type, const char* function = nullptr, int reset_reason = -1);

    // Crash statistics
    static uint32_t getCrashCount();
    static void resetCrashCount();

#ifdef ESP8266
    // Public for ESP8266 C callback access
    static void crashCallback(struct rst_info* rst_info, uint32_t stack, uint32_t stack_end);
#endif

private:
    static void storeCrashData(const CrashData& data);
    static uint8_t calculateChecksum(const CrashData& data);
    static bool validateCrashData(const CrashData& data);

    static bool _installed;
    static const uint32_t CRASH_MAGIC = 0xDEADBEEF;
};

// C-style callback for ESP8266
#ifdef ESP8266
extern "C" void custom_crash_callback(struct rst_info* rst_info, uint32_t stack, uint32_t stack_end);
#endif

#endif // ESP_CRASH_HANDLER_H