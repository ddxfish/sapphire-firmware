// main.c - Sapphire pendant: boot order.
//
//   settings -> serial console -> board (expander: the rail, I2S) -> light -> glass
//   -> camera probe -> WiFi -> sound (mic pipeline, wake word) -> card
//   -> when an address arrives: the HTTP server and the link to Sapphire
//
// Without settings the board still runs the console and the glass, so it can be
// set up and checked over USB alone.
#include <stdio.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"
#include "avatar.h"
#include "board.h"
#include "buttons.h"
#include "camera.h"
#include "capture.h"
#include "console.h"
#include "lastwords.h"
#include "light.h"
#include "link.h"
#include "logring.h"
#include "net.h"
#include "ota.h"
#include "power.h"
#include "server.h"
#include "settings.h"
#include "stats.h"
#include "storage.h"
#include "ui.h"
#include "wake.h"

static const char *TAG = "main";

static void on_network_up(void)
{
    ota_fine();
    server_start();
    link_start();
    light_cue("idle");
}

static void serial_console_io(void)
{
    // stdin on the CH340's UART, blocking reads, so fgets() works. A setup line
    // carries Sapphire's certificate in one go: a big receive buffer.
    uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0);   // internal RAM; a setup line is ~2 KB and drains as it lands
    uart_vfs_dev_use_driver(UART_NUM_0);
    uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CRLF);
    uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_LF);
    setvbuf(stdin, NULL, _IONBF, 0);
}

static void room(const char *when)
{
    ESP_LOGI(TAG, "%s: %u KB internal, %u KB PSRAM free", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

// The glass's task holds a lock every door needs: a glass whose tick stops for
// GLASS_STUCK_S restarts the board with every task's call chain kept. Asleep, the
// tick is meant to stop. This stack is internal: settings and sleep are done here.
#define GLASS_STUCK_S 10

static void watch_task(void *arg)
{
    for (int n = 0;; n++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!power_asleep() && ui_beat_age_ms() > GLASS_STUCK_S * 1000) lastwords_die("the glass stopped for 10 s");
        if (!power_asleep() && n % 5 == 0 && board_speaker_check() == 1) ui_heal();   // the expander found reset (its port 1 read wrong): every register back, then the panel; before, only her next sentence looked, and only at port 1 (scouts 2026-11-05)
        settings_tick();
        power_tick();
    }
}

// --- the buttons: BOOT, the halo, the two round buttons, the clock, the chat's name ---

// Her reply is going on when her own player talks, OR her stream carries her voice (0.2.0: the
// button went straight to listening over her, "mayhem", Krem 2026-11-03), OR her turn is being made.
static bool her_turn(void) { return server_talking() || avatar_voice() || light_turn_on(); }
// One button, four moments: she is talking or thinking -> stop her; my question is being recorded
// -> drop it, nothing sent; my question is on its way -> Sapphire drops the turn; else -> ask.
static void job_listen(void)
{
    if (her_turn()) server_stop_by_hand();
    else if (wake_recording()) wake_cancel();
    else if (wake_busy()) link_ask("stop");
    else wake_trigger();
}
static void job_stop(void)
{
    if (her_turn()) server_stop_by_hand();
    else if (wake_recording()) wake_cancel();
    else if (wake_busy()) link_ask("stop");
}
static void job_backup(void) { link_ask("backup"); }
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
static void job_status(void) { ui_status_open(); }
static void job_chats(void) { link_ask_chats(); }
static void job_sleep(void) { ui_toast("Sleeping. BOOT wakes me.", 2); power_sleep_soon(); }
static void job_fill(void) { board_fill_light(!board_fill_light_on()); }
static void job_face(void) { ui_face_toggle(); }

static const button_job_t JOBS[] = {
    {"listen", "Listen now, or stop her while she speaks", job_listen},
    {"stop", "Stop her speaking", job_stop},
    {"backup", "Back up to this board now", job_backup},
    {"light", "Status light on or off", job_light},
    {"screen", "Screen on or off", job_screen},
    {"wake", "Wake word on or off", job_wake},
    {"mute", "Speaker on or off", job_mute},
    {"louder", "Volume up a step", job_louder},
    {"quieter", "Volume down a step", job_quieter},
    {"status", "This pendant's status page", job_status},
    {"chats", "Pick the chat this pendant talks in", job_chats},
    {"sleep", "Sleep now (BOOT wakes it)", job_sleep},
    {"fill", "Fill light on or off", job_fill},
    {"face", "Hide her face, or show it again", job_face},
};
// BOOT is the talk key: a press listens (or stops her), held it sleeps. It is fed from
// here, not read by buttons.c, so the press that wakes the board is not also a talk.
// The places on the glass: the halo (middle), the two round buttons (bottom corners),
// the clock (top), the chat's name (bottom); the four corners and the top-left and
// top-right only exist for a game's taps and for Sapphire's bindings.
static const button_def_t BUTTONS[] = {
    {"boot", BUTTON_FED, {"listen", "sleep", NULL}},
    {"middle", BUTTON_TAP, {"listen", NULL, NULL}},
    {"bottom-left", BUTTON_TAP, {"wake", NULL, NULL}},
    {"bottom-right", BUTTON_TAP, {"mute", NULL, NULL}},
    {"top", BUTTON_TAP, {"status", NULL, NULL}},
    {"bottom", BUTTON_TAP, {"chats", NULL, NULL}},
    {"top-left", BUTTON_TAP, {"light", NULL, NULL}},
    {"top-right", BUTTON_TAP, {"screen", NULL, NULL}},
};

static void buttons_task(void *arg)
{
    bool ignore = false;                           // the press that woke the board: nothing until it is released
    bool was_asleep = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30));
        bool asleep = power_asleep();
        if (asleep) { was_asleep = true; continue; }
        if (was_asleep) { was_asleep = false; ignore = true; }
        bool down = board_boot_down();
        if (ignore && !down) ignore = false;
        buttons_feed(0, down && !ignore);
        buttons_poll();
    }
}

static void *json_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

void app_main(void)
{
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = json_alloc, .free_fn = free});
    logring_start();                     // every line from here on is readable at GET /log
    ESP_LOGI(TAG, "Sapphire pendant %s", PROJECT_VER);
    settings_load();
    lastwords_start();
    serial_console_io();
    console_start();
    stats_start();
    ota_start();
    room("at start");
    board_init(AUDIO_RATE);              // the expander brings the rail up here
    light_start();
    if (lastwords_strikes() >= 3) ESP_LOGE(TAG, "the glass stopped three starts in a row: left off this time");
    else if (board_glass_init()) ui_start();
    xTaskCreatePinnedToCore(watch_task, "watch", 4096, NULL, 6, NULL, 1);
    camera_start();
    room("after glass and camera");
    buttons_setup(BUTTONS, ui_ok() ? sizeof BUTTONS / sizeof BUTTONS[0] : 1, JOBS, sizeof JOBS / sizeof JOBS[0]);
    if (settings_ready()) net_start(on_network_up);
    room("after WiFi");
    audio_start();
    wake_start();
    capture_init();
    avatar_start();                      // her voice and her face as one stream (avatar.c)
    room("after sound");
    storage_start();
    audio_set_mute(settings.mute);       // the amplifier comes on here, unless it was left muted
    task_in_psram(buttons_task, "buttons", 4096, 4, 1);
    room("after card");
    if (!settings_ready()) {
        ESP_LOGW(TAG, "not set up yet: run setup_board.py setup");
        light_cue("starting");
    }
}
