idf_component_register(SRCS "main.c" "board.c" "settings.c" "console.c" "net.c" "light.c" "ui.c" "pages.c" "link.c" "server.c"
                            "speaker.c" "storage.c" "ota.c" "buttons.c"
                       INCLUDE_DIRS "."
                       REQUIRES app_update esp_lcd esp_wifi esp_event esp_netif nvs_flash esp_http_client esp_http_server esp-tls json
                                esp_timer esp_adc esp_driver_ledc esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_uart
                                vfs esp_driver_dac fatfs sdmmc esp_driver_sdspi mbedtls)

target_compile_definitions(${COMPONENT_LIB} PRIVATE PROJECT_VER="${PROJECT_VER}" FIRMWARE_MODEL="cyd28")
