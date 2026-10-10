idf_component_register(
    SRCS main.c settings.c console.c net.c server.c link.c light.c board.c audio.c wake.c storage.c ota.c stats.c lastwords.c ui.c buttons.c lv_mem_psram.c camera.c mww.cc
    INCLUDE_DIRS .
    LDFRAGMENTS linker.lf
    EMBED_FILES ../models/hey_jarvis.tflite
    REQUIRES app_update nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_http_client esp-tls
             esp_driver_i2c esp_driver_i2s esp_driver_usb_serial_jtag esp_timer json vfs spiffs
             fatfs sdmmc esp_driver_sdmmc mbedtls esp_lcd esp_driver_spi esp_driver_ledc esp_driver_gpio espcoredump)
target_compile_definitions(${COMPONENT_LIB} PRIVATE TF_LITE_STATIC_MEMORY TF_LITE_DISABLE_X86_NEON ESP_NN
                           PROJECT_VER="${PROJECT_VER}" FIRMWARE_MODEL="ai-speaker-mic")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
