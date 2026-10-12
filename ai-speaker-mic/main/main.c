// main.c - Sapphire satellite firmware: boot order.
//
//   settings -> USB console -> ring -> board (codecs) -> glass -> camera probe
//   -> WiFi -> sound (mic pipeline, wake word) -> card
//   -> when an address arrives: the HTTP server and the link to Sapphire
//
// Without settings the board still runs the console and the sound self test,
// so it can be set up and checked over USB alone.
#include <stdio.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"
#include "board.h"
#include "buttons.h"
#include "camera.h"
#include "console.h"
#include "lastwords.h"
#include "light.h"
#include "link.h"
#include "net.h"
#include "ota.h"
#include "server.h"
#include "settings.h"
#include "stats.h"
#include "storage.h"
#include "ui.h"
#include "wake.h"

static const char *TAG = "main";

static void on_network_up(void)
{
    ota_fine();                          // a program on trial that reaches WiFi is kept
    server_start();
    link_start();
    light_cue("idle");
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
    ESP_LOGI(TAG, "%s: %u KB internal, %u KB PSRAM free", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

// The glass's task holds a lock every door needs. When its tick stops for
// GLASS_STUCK_S the board writes down where every task stands and restarts:
// a minute of no glass beats a night of it (2026-10-08, frozen from 21:48
// until a hand found it). Core 1 and above LVGL's priority, so a glass that
// spins is caught mid-step, not only one that waits.
#define GLASS_STUCK_S 10

static void watch_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (ui_beat_age_ms() > GLASS_STUCK_S * 1000) lastwords_die("the glass stopped for 10 s");
        settings_tick();                 // this stack is internal: what a slider or a key changed is written to flash here
    }
}

// --- the buttons (buttons.h): BOOT, the keys, the glass's corners. Each does one of these jobs, or what Sapphire set. ---

static void job_listen(void)
{
    if (server_talking() || light_turn_on()) server_stop_by_hand();  // while her turn goes on a press only ever stops her
    else wake_trigger();
}
static void job_stop(void) { if (server_talking() || light_turn_on()) server_stop_by_hand(); }  // nothing to stop: nothing is refused
static void job_backup(void) { link_ask("backup"); }           // Sapphire makes it and sends it here; her answer shows on the glass
static void job_light(void) { light_toggle_dark(); }
static void job_screen(void) { ui_screen_power(!ui_screen_on()); }
static void job_wake(void) { wake_enable(!wake_enabled()); }
static void job_mute(void) { audio_set_mute(!audio_muted()); }
static void volume_by(int step)
{
    audio_set_volume(audio_volume() + step);
    char text[24];
    snprintf(text, sizeof text, "Volume %d%%", audio_volume());
    ui_toast(text, 2);
}
static void job_louder(void) { volume_by(settings.vol_step); }
static void job_quieter(void) { volume_by(-settings.vol_step); }

static const button_job_t JOBS[] = {
    {"listen", "Listen now, or stop her while she speaks", job_listen},
    {"stop", "Stop her speaking", job_stop},
    {"backup", "Back up to this board now", job_backup},
    {"light", "Ring on or off", job_light},
    {"screen", "Screen on or off", job_screen},
    {"wake", "Wake word on or off", job_wake},
    {"mute", "Speaker on or off", job_mute},
    {"louder", "Volume up a step", job_louder},
    {"quieter", "Volume down a step", job_quieter},
};
// BOOT, the three keys on the expander, then the glass's four corners (ui.c taps them by name).
// Left alone, the real buttons are a remote for a board with no glass: volume at the two ends
// (BOOT down, Key3 up), Key1 listens or stops her, Key2 is the ring, held the wake word, twice
// the speaker (Krem's own set after a day of use, 2026-10-11).
#define FIRST_KEY 1
static const button_def_t BUTTONS[] = {
    {"boot", BOARD_BOOT_BUTTON, {"quieter", NULL, NULL}},
    {"key1", BUTTON_FED, {"listen", NULL, NULL}},
    {"key2", BUTTON_FED, {"light", "wake", "mute"}},
    {"key3", BUTTON_FED, {"louder", NULL, NULL}},
    {"top-left", BUTTON_TAP, {"light", NULL, NULL}},
    {"top-right", BUTTON_TAP, {"screen", NULL, NULL}},
    {"bottom-left", BUTTON_TAP, {"wake", NULL, NULL}},
    {"bottom-right", BUTTON_TAP, {"mute", NULL, NULL}},
};

static void buttons_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30));
        int keys = board_keys();                       // one small read of the shared bus
        for (int k = 0; keys >= 0 && k < BOARD_KEYS; k++) buttons_feed(FIRST_KEY + k, keys >> k & 1);
        buttons_poll();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Sapphire satellite %s", PROJECT_VER);
    settings_load();
    lastwords_start();                   // why the last boot ended, before anything can end this one
    usb_console_io();
    console_start();
    stats_start();                       // where the time goes, from the first second
    ota_start();                         // a program on trial is kept once WiFi joins or 90 s pass
    light_start();
    room("at start");
    // The board (I2C, expander, codecs), then the glass and the camera's probe,
    // all BEFORE WiFi: the probe wants 20 KB of internal RAM for a moment, and
    // with WiFi and the link up it once left 1 KB (2026-10-07). Then WiFi, then
    // the sound pipeline: WiFi's buffers must be internal, and the AFE would
    // otherwise take that room first (a boot loop, 2026-09-29).
    board_init(AUDIO_RATE);
    if (lastwords_strikes() >= 3) ESP_LOGE(TAG, "the glass stopped three starts in a row: left off this time");
    else if (board_glass_init()) ui_start();   // the glass (0.4.0): the panel on SPI2, the touch chip on the shared bus
    xTaskCreatePinnedToCore(watch_task, "watch", 4096, NULL, 6, NULL, 1);   // 4 KB: it writes the settings to flash too
    camera_start();                    // the eye (0.5.0): woken once to say who it is, then powered down
    room("after glass and camera");
    // before WiFi: what Sapphire set the presses to arrives the moment her stream opens, and
    // with no buttons known yet it was dropped (certain on a first start, the model store's format takes 20 s)
    buttons_setup(BUTTONS, ui_ok() ? 8 : FIRST_KEY + BOARD_KEYS, JOBS, sizeof JOBS / sizeof JOBS[0]);   // no glass, no corners
    if (settings_ready()) net_start(on_network_up);
    room("after WiFi");
    audio_start();
    wake_start();
    room("after sound");
    storage_start();                   // the card: its CS line is on the expander
    if (settings.mute) audio_set_mute(true);            // as it was left
    task_in_psram(buttons_task, "buttons", 4096, 4, 1);   // its jobs flip flags and never write flash
    room("after card");
    if (!settings_ready()) {
        ESP_LOGW(TAG, "not set up yet: run setup_board.py setup");
        light_cue("starting");
    }
}
