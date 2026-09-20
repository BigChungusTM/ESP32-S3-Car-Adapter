idf_component_register(SRCS ipod_usb.c iap.c
                       INCLUDE_DIRS include
                       REQUIRES espressif__tinyusb usb freertos heap log esp_system esp_timer nvs_flash)
