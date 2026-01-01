# Bug Report: esp-iot-log

## Issues Found and Fixed

### 1. Header Size Mismatch in Python Receiver (FIXED)
**File:** `python/esp_iot_log_receiver.py`

**Problem:** The receiver expected 19-byte headers but the C struct is 18 bytes:
- `magic` (2) + `version` (1) + `device_id` (8) + `timestamp` (4) + `log_type` (1) + `length` (2) = 18 bytes

**Symptoms:** `Error parsing message: unpack requires a buffer of 18 bytes`

**Fix:** Changed lines 425, 428, 444, 448 from 19 to 18.

### 2. Library Dependencies Conflict (FIXED)
**File:** `library.json`

**Problem:** Dependencies on `WiFi @ *` and `ESPmDNS` caused PlatformIO to install the wrong WiFi library for ESP8266, resulting in multiple definition linker errors.

**Fix:** Removed external dependencies since ESP8266/ESP32 frameworks provide WiFi and mDNS natively.

---

## Suggestions for Improvement

### 1. ESP8266 Heap Tracking
**File:** `src/ESPIoTLog.cpp:370-371`

Currently sets `heap_largest_block = 0` for ESP8266, but ESP8266 supports `ESP.getMaxFreeBlockSize()`.

### 2. Serial Output Toggle
The `logf()` function always outputs to Serial. Add `setSerialEnabled(bool)` for users who want network-only logging.

### 3. Memory Optimization for ESP8266
- 256-byte buffer in `logf()` (line 195)
- 1024-byte stack buffer in `sendLogMessage()` (line 321)

Consider smaller defaults for ESP8266 (128 bytes).

### 4. mDNS Conflict Prevention
`begin()` calls `MDNS.begin()` which may conflict if the app already started mDNS. Consider making this optional or checking if already running.

### 5. Missing Struct Fields
The `SystemTelemetry` struct in the header doesn't include `advanced`, `wifi_advanced`, `chip_info` fields referenced in ESP32 code paths (lines 429-525).
