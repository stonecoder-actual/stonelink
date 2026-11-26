idf_component_register(SRCS "main.c" "local_storage.c" "collection.c" "power_cycle.c" "capture_metadata.c" "archive_naming.c" "media_group.c" "transfer_queue.c" "transfer_queue_io.c" INCLUDE_DIRS "."
                       REQUIRES usb fatfs esp_timer esp_system esp_hw_support sdmmc esp_driver_sdmmc esp_driver_gpio nvs_flash mbedtls json)
