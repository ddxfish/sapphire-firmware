// main.c - Sapphire's backup stick (LilyGO T-Dongle-S3). Boot order:
//
//   settings -> USB console -> board (panel, LED) -> the card -> screen -> light
//   -> WiFi -> when an address arrives: the HTTP server and the link to Sapphire
//
// The stick has one job: hold sealed backups on its card and say, on its LED
// and its two lines of screen, whether they are safe. Without settings it
// still shows the screen and runs the console, so it can be set up over USB.
#include <stdio.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "buttons.h"
#include "console.h"
#include "light.h"
#include "link.h"
#include "net.h"
#include "ota.h"
#include "server.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "main";

// BOOT's jobs (buttons.h); they run on the screen's task. A press on a dark
// screen only lights it: the first touch of a stick in a port is a look.
static void job_backup(void) { if (!ui_wake()) link_ask("backup"); }
static void job_screen(void) { if (!ui_wake()) ui_sleep(); }

static const button_job_t JOBS[] = {
    {"backup", "Back up to this stick now", job_backup},
    {"screen", "Screen on or off", job_screen},
};
static const button_def_t BUTTONS[] = {{"boot", BOARD_BOOT_BUTTON, {"backup", "screen", NULL}}};

static void on_network_up(void)
{
    ota_fine();                          // a program on trial that reaches WiFi is kept
    server_start();
    link_start();
}

static void usb_console_io(void)
{
    // stdin/stdout on the USB serial port, blocking reads, so fgets() works
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 4096;         // a setup line carries Sapphire's certificate in one go
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
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
    ESP_LOGI(TAG, "Sapphire backup stick %s", PROJECT_VER);
    settings_load();
    usb_console_io();
    console_start();
    ota_start();                         // a program on trial is kept once WiFi joins or 90 s pass
    room("at start");
    board_init();
    storage_start();                     // the card, when one is in and FAT32; the check of its backups starts here
    buttons_setup(BUTTONS, 1, JOBS, sizeof JOBS / sizeof JOBS[0]);
    light_start();
    ui_start();
    light_cue("idle");
    room("after screen and card");
    if (settings_ready()) {
        net_start(on_network_up);
        room("after WiFi");
    } else {
        ESP_LOGW(TAG, "not set up yet: Settings > Devices > New Board in Sapphire");
    }
}
