idf_component_register(SRCS "main.c" "ble_central.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_driver_rmt esp_driver_gpio
             esp_wifi nvs_flash esp_http_client esp_event esp-sr bt)
