idf_component_register(SRCS "main.c" "local_storage.c" INCLUDE_DIRS "."
                       REQUIRES usb fatfs esp_timer esp_system sdmmc esp_driver_sdmmc)
