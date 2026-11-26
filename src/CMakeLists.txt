idf_component_register(SRCS "main.c" "local_storage.c" "collection.c" "power_cycle.c" INCLUDE_DIRS "."
                       REQUIRES usb fatfs esp_timer esp_system sdmmc esp_driver_sdmmc esp_driver_gpio nvs_flash)
