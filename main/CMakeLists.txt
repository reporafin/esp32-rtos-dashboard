idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver freertos log esp_hw_support lvgl esp_lvgl_port nvs_flash esp_wifi esp_netif esp_event lwip esp_http_client esp_timer mbedtls esp-tls)