# ESP IoT Log - Zephyr Port
#
# Usage as a Zephyr module (in your app's CMakeLists.txt or west.yml):
#   list(APPEND ZEPHYR_EXTRA_MODULES /path/to/esp-iot-log/zephyr)
#
# Or include directly:
#   target_sources(app PRIVATE /path/to/esp-iot-log/zephyr/iot_log_zephyr.c)
#   target_include_directories(app PRIVATE /path/to/esp-iot-log/zephyr/include)

if(CONFIG_IOT_LOG)
  zephyr_library()
  zephyr_library_sources(iot_log_zephyr.c)
  zephyr_library_sources(log_backend_iot.c)
  zephyr_include_directories(include)
endif()
