idf_component_register(SRCS "app_main.c" "contract.c"
    INCLUDE_DIRS "."
    REQUIRES briqon_core sensor_adapter esp_wifi esp_netif esp_event nvs_flash
             mqtt json esp_timer esp_system esp_driver_gpio mbedtls lwip)
