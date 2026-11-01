idf_component_register(SRCS "sensor_adapter.c" INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio ds18b20 onewire_bus briqon_core esp_rom freertos log)
