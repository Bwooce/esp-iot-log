idf_component_register(
    SRCS "esp_iot_log.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_wifi mdns lwip espcoredump esp_common
)
