idf_component_register(
    SRCS main.c avatar.c logring.c auth.c settings.c console.c net.c server.c link.c light.c board.c audio.c wake.c capture.c storage.c ota.c stats.c lastwords.c ui.c buttons.c lv_mem_psram.c camera.c power.c mww.cc
    INCLUDE_DIRS .
    LDFRAGMENTS linker.lf
    EMBED_FILES ../models/hey_jarvis.tflite
    REQUIRES app_update nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_http_client esp-tls
             esp_driver_i2c esp_driver_i2s esp_driver_uart esp_timer json vfs spiffs esp_adc esp_pm
             fatfs sdmmc esp_driver_sdmmc esp_driver_sdspi mbedtls esp_lcd esp_driver_spi esp_driver_ledc esp_driver_gpio espcoredump
             esp32-camera)                       # the speaker board's patched copy (its cache fix), in components/
target_compile_definitions(${COMPONENT_LIB} PRIVATE TF_LITE_STATIC_MEMORY TF_LITE_DISABLE_X86_NEON ESP_NN
                           PROJECT_VER="${PROJECT_VER}" FIRMWARE_MODEL="pendant")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
