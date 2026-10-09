// main.c - Sapphire pocket: a chat with her in your pocket. Boot order:
//
//   settings -> serial console -> board (panel, touch, LED) -> screen -> light
//   -> WiFi -> when an address arrives: the HTTP server and the link to Sapphire
//
// Without settings the board still shows the screen and runs the console, so
// it can be set up and checked over USB alone.
#include <stdio.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "console.h"
#include "light.h"
#include "link.h"
#include "net.h"
#include "ota.h"
#include "server.h"
#include "settings.h"
#include "speaker.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "main";

static void on_network_up(void)
{
    ota_fine();                          // a program on trial that reaches WiFi is kept
    server_start();
    link_start();
    light_cue("idle");
}

static void serial_console_io(void)
{
    // stdin on the CH340's UART, blocking reads, so fgets() works. A setup
    // line carries Sapphire's certificate in one go: a big receive buffer.
    uart_driver_install(UART_NUM_0, 8192, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(UART_NUM_0);
    uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CRLF);
    uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
    setvbuf(stdin, NULL, _IONBF, 0);
}

static void room(const char *when)
{
    ESP_LOGI(TAG, "%s: %u KB free, largest block %u KB", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Sapphire pocket %s", PROJECT_VER);
    settings_load();
    serial_console_io();
    console_start();
    ota_start();                         // a program on trial is kept once WiFi joins or 90 s pass
    room("at start");
    board_init();
    light_start();
    ui_start();
    room("after screen");
    speaker_start();                   // her voice through the amplifier (0.2.0)
    storage_start();                   // the card, when one is in and FAT32
    room("after speaker and card");
    if (settings_ready()) {
        net_start(on_network_up);
        room("after WiFi");
    } else {
        ESP_LOGW(TAG, "not set up yet: run setup_board.py setup");
        light_cue("starting");
        ui_status("not set up");
        ui_toast("Not set up yet. Run setup_board.py setup over USB.", 60);
    }
}
