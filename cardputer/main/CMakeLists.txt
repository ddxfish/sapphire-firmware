idf_component_register(
    SRCS main.c settings.c net.c link.c console.c light.c board.c screen.c server.c walkie.c shell.c storage.c ir.c
    INCLUDE_DIRS .
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_client esp-tls
             esp_driver_usb_serial_jtag esp_timer esp_hw_support json
             esp_driver_i2s esp_driver_i2c esp_http_server
             esp_lcd esp_driver_spi esp_driver_ledc esp_driver_gpio
             vfs fatfs sdmmc esp_driver_sdspi esp_driver_rmt)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           PROJECT_VER="${PROJECT_VER}" FIRMWARE_MODEL="cardputer")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
