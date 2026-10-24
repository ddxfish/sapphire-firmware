// main.c - Sapphire Cardputer satellite: boot order (P1).
//   settings -> USB console -> light -> WiFi -> (on an address) the link to Sapphire
// Without settings the board still runs the console, so it is set up over USB alone.
#include <stdio.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "console.h"
#include "ir.h"
#include "light.h"
#include "link.h"
#include "net.h"
#include "server.h"
#include "settings.h"
#include "shell.h"
#include "storage.h"

static const char *TAG = "main";

// On an address (idempotent; a WiFi rejoin may run it again): hold the link open.
static void on_network_up(void)
{
    link_start();
    server_start();                  // Sapphire POSTs her spoken replies to us (/audio/speak)
    light_cue("idle");
}

static void usb_console_io(void)
{
    // stdin/stdout on the USB serial port, blocking reads, so fgets() works
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 4096;         // a setup line can carry Sapphire's certificate in one go
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    setvbuf(stdin, NULL, _IONBF, 0);
}

static void room(const char *when)
{
    ESP_LOGI(TAG, "%s: %u KB internal free", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Sapphire Cardputer %s", PROJECT_VER);
    settings_load();
    usb_console_io();
    console_start();
    light_start();
    room("at start");
    if (!board_init(16000)) ESP_LOGW(TAG, "board init stopped: %s", board_trouble());
    room("after board");
    if (!ir_init()) ESP_LOGW(TAG, "IR blaster did not start");
    // The SD card is mounted on demand, not at boot: FATFS + the SPI driver cost
    // ~37 KB, and the Walkie-Talkie (the primary app) needs that RAM and never
    // touches the card. `sd` on the console and the disk-using apps mount it when
    // they need it (storage_start) and free it on the way out (storage_stop).
    shell_start();                   // the launcher + apps (owns the keyboard, runs Walkie-Talkie)
    if (settings_ready()) net_start(on_network_up);
    else ESP_LOGW(TAG, "not set up yet: console `show`, `setup {json}`, `restart`");
    room("after WiFi start");
}
