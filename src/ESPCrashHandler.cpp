/**
 * ESP Crash Handler - Implementation
 * Copyright (c) 2026 Bruce Fitzsimons
 */

#include "ESPCrashHandler.h"

#ifdef ESP32
  #include <soc/rtc_cntl_reg.h>
  #include <soc/rtc.h>
#elif defined(ESP8266)
  extern "C" {
    #include <ets_sys.h>
  }
#endif

bool ESPCrashHandler::_installed = false;

// RTC memory storage (survives warm resets)
#ifdef ESP32
  RTC_DATA_ATTR CrashData rtc_crash_data;
  RTC_DATA_ATTR uint32_t rtc_crash_count = 0;
#elif defined(ESP8266)
  // ESP8266 RTC memory is at 0x60001100-0x60001200 (256 bytes)
  #define RTC_MEM_CRASH_DATA_ADDR 64  // Offset in RTC memory
  #define RTC_MEM_CRASH_COUNT_ADDR 96
#endif

void ESPCrashHandler::install() {
    if (_installed) return;

#ifdef ESP32
    // Register panic hook
    esp_register_shutdown_handler(&panicHandler);

    // Install exception handlers if available
    // Note: More advanced exception handling requires ESP-IDF level access

#elif defined(ESP8266)
    // Set custom crash callback
    system_set_os_print(0); // Disable default crash output

    // The custom_crash_callback is called from C, so we use a wrapper
    // This is set up in the extern "C" function at bottom of file
#endif

    _installed = true;
}

void ESPCrashHandler::uninstall() {
    if (!_installed) return;

#ifdef ESP32
    // ESP32 doesn't provide unregister for shutdown handlers
    // They persist until next reset
#elif defined(ESP8266)
    system_set_os_print(1); // Re-enable default crash output
#endif

    _installed = false;
}

bool ESPCrashHandler::hasCrashData() {
#ifdef ESP32
    return validateCrashData(rtc_crash_data);
#elif defined(ESP8266)
    CrashData data;
    if (!system_rtc_mem_read(RTC_MEM_CRASH_DATA_ADDR, &data, sizeof(data))) {
        return false;
    }
    return validateCrashData(data);
#endif
}

bool ESPCrashHandler::getCrashData(CrashData& data) {
#ifdef ESP32
    if (!validateCrashData(rtc_crash_data)) {
        return false;
    }
    data = rtc_crash_data;
    return true;
#elif defined(ESP8266)
    if (!system_rtc_mem_read(RTC_MEM_CRASH_DATA_ADDR, &data, sizeof(data))) {
        return false;
    }
    return validateCrashData(data);
#endif
}

void ESPCrashHandler::clearCrashData() {
#ifdef ESP32
    memset(&rtc_crash_data, 0, sizeof(rtc_crash_data));
#elif defined(ESP8266)
    CrashData empty;
    memset(&empty, 0, sizeof(empty));
    system_rtc_mem_write(RTC_MEM_CRASH_DATA_ADDR, &empty, sizeof(empty));
#endif
}

void ESPCrashHandler::recordCrash(crash_type_t type, const char* function) {
    CrashData crash;
    memset(&crash, 0, sizeof(crash));

    crash.magic = CRASH_MAGIC;
    crash.timestamp = millis();
    crash.crash_type = type;
    crash.heap_free = ESP.getFreeHeap() / 1024; // Store as KB to fit in uint16_t

#ifdef ESP32
    crash.reset_reason = esp_reset_reason();

    // Get stack pointer (simplified)
    uint32_t* sp;
    asm volatile ("mov %0, sp" : "=r" (sp));
    crash.stack_ptr = (uint32_t)sp;

    // Try to get PC from return address
    crash.pc = (uint32_t)__builtin_return_address(0);

#elif defined(ESP8266)
    crash.reset_reason = ESP.getResetInfoPtr()->reason;

    // ESP8266 stack info (limited)
    // Use stack pointer approximation since _estack isn't available
    uint32_t stack_var;
    crash.stack_ptr = (uint32_t)&stack_var;
    crash.pc = 0; // Not easily accessible
#endif

    // Copy function name if provided
    if (function) {
        strncpy(crash.last_function, function, sizeof(crash.last_function) - 1);
        crash.last_function[sizeof(crash.last_function) - 1] = '\0';
    }

    crash.checksum = calculateChecksum(crash);

    storeCrashData(crash);

    // Increment crash counter
#ifdef ESP32
    rtc_crash_count++;
#elif defined(ESP8266)
    uint32_t count = getCrashCount() + 1;
    system_rtc_mem_write(RTC_MEM_CRASH_COUNT_ADDR, &count, sizeof(count));
#endif
}

uint32_t ESPCrashHandler::getCrashCount() {
#ifdef ESP32
    return rtc_crash_count;
#elif defined(ESP8266)
    uint32_t count = 0;
    system_rtc_mem_read(RTC_MEM_CRASH_COUNT_ADDR, &count, sizeof(count));
    return count;
#endif
}

void ESPCrashHandler::resetCrashCount() {
#ifdef ESP32
    rtc_crash_count = 0;
#elif defined(ESP8266)
    uint32_t count = 0;
    system_rtc_mem_write(RTC_MEM_CRASH_COUNT_ADDR, &count, sizeof(count));
#endif
}

void ESPCrashHandler::storeCrashData(const CrashData& data) {
#ifdef ESP32
    rtc_crash_data = data;
#elif defined(ESP8266)
    system_rtc_mem_write(RTC_MEM_CRASH_DATA_ADDR, (void*)&data, sizeof(data));
#endif
}

uint8_t ESPCrashHandler::calculateChecksum(const CrashData& data) {
    uint8_t checksum = 0;
    const uint8_t* bytes = (const uint8_t*)&data;

    // Checksum all bytes except the checksum field itself
    for (size_t i = 0; i < sizeof(data) - 1; i++) {
        checksum ^= bytes[i];
    }

    return checksum;
}

bool ESPCrashHandler::validateCrashData(const CrashData& data) {
    if (data.magic != CRASH_MAGIC) {
        return false;
    }

    // Create a copy and calculate checksum
    CrashData temp = data;
    temp.checksum = calculateChecksum(temp);

    return temp.checksum == data.checksum;
}

#ifdef ESP32
void ESPCrashHandler::panicHandler() {
    recordCrash(CRASH_TYPE_PANIC, "panic_handler");
}

void ESPCrashHandler::exceptionHandler() {
    recordCrash(CRASH_TYPE_EXCEPTION, "exception_handler");
}

#elif defined(ESP8266)
void ESPCrashHandler::crashCallback(struct rst_info* rst_info, uint32_t stack, uint32_t stack_end) {
    crash_type_t type = CRASH_TYPE_UNKNOWN;

    if (rst_info) {
        switch (rst_info->reason) {
            case REASON_WDT_RST:
                type = CRASH_TYPE_WATCHDOG;
                break;
            case REASON_EXCEPTION_RST:
                type = CRASH_TYPE_EXCEPTION;
                break;
            case REASON_EXT_SYS_RST:
                type = CRASH_TYPE_BROWNOUT;
                break;
            default:
                type = CRASH_TYPE_UNKNOWN;
                break;
        }
    }

    recordCrash(type, "crash_callback");
}

// C wrapper function for ESP8266 crash callback
extern "C" void custom_crash_callback(struct rst_info* rst_info, uint32_t stack, uint32_t stack_end) {
    ESPCrashHandler::crashCallback(rst_info, stack, stack_end);
}
#endif