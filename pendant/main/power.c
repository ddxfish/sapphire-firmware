// power.c - see power.h.
#include "power.h"
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lastwords.h"
#include "audio.h"
#include "avatar.h"
#include "board.h"
#include "buttons.h"
#include "camera.h"
#include "capture.h"
#include "light.h"
#include "net.h"
#include "server.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"
#include "wake.h"

static const char *TAG = "power";
static volatile bool asked, restart_asked, asleep;
static int64_t woke_us;
static int sleeps;
static bool no_sleep, no_sleep_said;    // the last reset was a brownout (at a wake, the supply sagged): no sleep until a power cycle, or the next wake is the same dark trap (Krem, 2026-11-04)
#define NO_SLEEP_MAGIC 0x6e6f7a7a
static RTC_NOINIT_ATTR uint32_t no_sleep_rtc;   // NO_SLEEP_MAGIC while that holds: an OTA, a /power restart or a crash in between kept the guard before only one reset deep (scouts 2026-11-05); a power-on (the RESET button too) clears it

bool power_no_sleep(void) { return no_sleep; }

void power_sleep_soon(void)
{
    if (no_sleep) { ui_toast("No sleep: the last wake browned out. Power-cycle to allow it.", 6); return; }
    asked = true;
}
void power_restart_soon(void) { restart_asked = true; }
bool power_asleep(void) { return asleep; }
bool power_sleep_pending(void) { return asked; }
int power_awake_s(void) { return (int)((esp_timer_get_time() - woke_us) / 1000000); }
int power_sleeps(void) { return sleeps; }

// Nothing that would be cut short: her reply, a question, a picture arriving, a backup landing.
static bool busy(void)
{
    return server_talking() || avatar_voice() || wake_busy() || light_turn_on() || capture_active() || storage_busy() || storage_checking() || ui_picture_on();   // the card's check too: sleep cut it mid-read and a good backup was marked damaged (scouts 2026-11-05)
}

static void go_to_sleep(void)
{
    sleeps++;
    ESP_LOGI(TAG, "going to sleep: the rail off, WiFi off; BOOT wakes it");
    settings_flush();
    light_cue("off");
    ui_sleep(true);                                // LVGL's lock taken and held: no flush in flight when the panel loses power
    avatar_clear();                                // her stream: what is queued goes, the feeder stops; the next packet after the wake starts it afresh
    wake_pause(true);
    audio_suspend(true);
    board_amplifier(false);
    camera_sleep();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    net_sleep(true);                               // after the stop's events: the rejoin they arm, and mDNS's timer, must not run into the sleep (every missed period replays at the wake; ui_sleep stops LVGL's tick for the same reason, 2026-11-05)
    board_led(0, 0, 0);
    board_rail(false);
    asleep = true;
    for (int i = 0; i < 150 && !gpio_get_level(BOARD_BOOT_BUTTON); i++) vTaskDelay(pdMS_TO_TICKS(20));   // the hold that asked for this sleep may still be down: a level wake would fire at once, a whole wake for nothing (scouts 2026-11-05)
    gpio_wakeup_enable(BOARD_BOOT_BUTTON, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    uart_set_wakeup_threshold(UART_NUM_0, 3);      // a line typed on the console wakes it too (its first characters are lost)
    esp_sleep_enable_uart_wakeup(UART_NUM_0);
    for (;;) {
        esp_light_sleep_start();
        esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();
        if (why == ESP_SLEEP_WAKEUP_GPIO || why == ESP_SLEEP_WAKEUP_UART) break;
    }
    gpio_wakeup_disable(BOARD_BOOT_BUTTON);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    ESP_LOGI(TAG, "awake");
    woke_us = esp_timer_get_time();
    buttons_spend(1000);                           // BOOT, held until the picture showed, is not a long press (= sleep): it is the wake. A second's worth: the glass is back in ~0.9 s, and 2 s ate a real press for a second after it (scouts 2026-11-05)
    // One thing at a time: the rail, the glass, the amplifier, then the radio. All at once, the
    // supply sagged and the chip reset on brownout at the wake (reset "brownout" after Krem's
    // hold-to-wake, 2026-11-03: a picture for a second, then dark). The radio's first burst is
    // the biggest draw, so it comes last, half a second after the glass.
    if (!board_rail(true)) ESP_LOGE(TAG, "the rail did not come on at the wake: the expander did not take the write");   // tried thrice inside; silent before, with the log saying "awake" over a dark glass (scouts 2026-11-05)
    vTaskDelay(pdMS_TO_TICKS(120));
    if (!board_glass_wake()) {
        ESP_LOGW(TAG, "the glass did not come back: once more");
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!board_glass_wake()) ESP_LOGE(TAG, "the glass did not come back");
    }
    asleep = false;
    ui_sleep(false);
    ui_toast("Waking: WiFi and her link take a few seconds", 5);   // a tap before they are back goes nowhere; say so (Krem's "unstable" wake, 2026-11-03)
    vTaskDelay(pdMS_TO_TICKS(150));
    audio_suspend(false);
    board_amplifier(!audio_muted());
    wake_pause(false);
    light_cue("idle");
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_wifi_start();                              // net.c's handler joins again on STA_START
    esp_wifi_set_max_tx_power(44);                 // 11 dBm while it joins: the first bursts at full power are the wake's biggest draw; net.c restores full power once the IP lands
    net_sleep(false);
}

void power_tick(void)
{
    if (restart_asked) {
        restart_asked = false;
        settings_flush();
        ESP_LOGI(TAG, "restarting");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    if (!no_sleep_said) {
        no_sleep_said = true;
        no_sleep = !strcmp(lastwords_reset(), "brownout") || (no_sleep_rtc == NO_SLEEP_MAGIC && strcmp(lastwords_reset(), "power on"));
        no_sleep_rtc = no_sleep ? NO_SLEEP_MAGIC : 0;
        if (no_sleep) {
            ESP_LOGW(TAG, "the last reset was a brownout: sleep is off until a power cycle (the wake's load sagged the supply)");
            ui_toast("Brownout at the last wake: no sleep until power-cycled", 8);
        }
    }
    if (!asked && !no_sleep && settings.sleep_after_min && ui_ok() && !busy()
        && ui_idle_ms() > (int64_t)settings.sleep_after_min * 60000) asked = true;
    if (asked) {
        asked = false;
        if (busy()) { asked = true; return; }      // again next second
        if (!ui_ok()) { ESP_LOGW(TAG, "no glass up: sleep is only for a board with its glass"); return; }
        go_to_sleep();
    }
}
