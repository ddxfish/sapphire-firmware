// power.c - see power.h.
#include "power.h"
#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_rom_crc.h"
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

// The sleep trail: how far the last sleep and its wake got, in RTC memory behind a CRC, so it reads
// after ANY reset, the RESET button included (RTC SRAM keeps its bytes while the board has power;
// lastwords distrusts RTC after a "power on" because it has no CRC to go by; the bootloader's own
// rtc_retain_mem is kept this way). Written at every step of a sleep and a wake. A board that comes
// back dark and gets RESET by hand still says what it was doing: the 43 h sleep whose BOOT press
// did nothing (2026-11-08) left no readable word anywhere, the PSRAM ring rotted in the reset.
#define TRAIL_MAGIC 0x74726c33           // changes with the struct below: an older trail reads as fresh, once
#define BEAT_S 1800                      // asleep, the chip wakes for a few ms every half hour to count itself alive, then sleeps on
typedef struct {
    uint32_t magic, sleeps;              // sleeps since the trail began (a power cycle)
    uint32_t loops, rejects, beats;      // the last sleep: light-sleep calls, of them rejected, heartbeats
    uint32_t slept_s;                    // the last sleep: seconds from its start to its last entry
    uint8_t step, cause;                 // the last sleep: how far it got (STEP_*), what woke it (esp_sleep_wakeup_cause_t)
    uint16_t pad;
    uint32_t up_s;                       // this boot: seconds up, stamped every second (a brownout boot reads how long the life before it lasted)
    uint32_t browns, naps;               // brownout boots in a row; deep sleeps the dead-battery guard took since the power cycle
    uint32_t crc;
} trail_t;

// The dead battery (power.h). A brownout within DOUBT_S of the boot before it is the second of a
// row; the guard deep-sleeps NAP_S, doubled per brownout in the row, up to NAP_MAX_S, and tries
// once at each timer wake with the amplifier off. The brownout handler ends in a software reset
// (IDF brownout.c: esp_rom_software_reset_system), so RTC memory, this trail with it, reads through
// one; a cell that falls far enough for a power-on reset instead is beyond this guard (the trail is
// fresh at every lap then), and only a protected cell helps. Level 7 in sdkconfig is 2.44 V on
// this chip.
#define DOUBT_S 120                      // a boot that lives this long has a supply that carries it
#define NAP_S 60
#define NAP_MAX_S 1800
static bool doubt;                       // this boot is the guard's try
enum { STEP_NONE, STEP_ASLEEP, STEP_WOKE, STEP_RAIL, STEP_GLASS, STEP_UI, STEP_SOUND, STEP_RADIO, STEP_CARD, STEP_DONE };
static const char *const STEP_NAMES[] = {"none", "asleep", "woke", "rail", "glass", "ui", "sound", "radio", "card", "done"};
static RTC_NOINIT_ATTR trail_t trail;
static trail_t before;                   // the trail as this boot found it
static bool before_ok;
static int64_t slept_from_us;

static uint32_t trail_crc(const trail_t *t) { return esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)t, offsetof(trail_t, crc)); }
static void trail_save(void) { trail.crc = trail_crc(&trail); }
static void trail_step(int step)
{
    trail.step = step;
    trail.slept_s = (uint32_t)((esp_timer_get_time() - slept_from_us) / 1000000);   // esp_timer is stepped by the slept time at every exit from light sleep
    trail_save();
}
static const char *step_name(unsigned step) { return STEP_NAMES[step < sizeof STEP_NAMES / sizeof *STEP_NAMES ? step : 0]; }
static const char *cause_name(unsigned cause)
{
    return cause == ESP_SLEEP_WAKEUP_GPIO ? "BOOT" : cause == ESP_SLEEP_WAKEUP_UART ? "console" : cause == ESP_SLEEP_WAKEUP_TIMER ? "beat" : cause ? "other" : "none";
}

static cJSON *trail_json(const trail_t *t)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "step", step_name(t->step));
    cJSON_AddStringToObject(j, "cause", cause_name(t->cause));
    cJSON_AddNumberToObject(j, "slept_s", t->slept_s);
    cJSON_AddNumberToObject(j, "beats", t->beats);
    cJSON_AddNumberToObject(j, "loops", t->loops);
    cJSON_AddNumberToObject(j, "rejects", t->rejects);
    cJSON_AddNumberToObject(j, "sleeps", t->sleeps);
    cJSON_AddNumberToObject(j, "up_s", t->up_s);
    cJSON_AddNumberToObject(j, "brownouts", t->browns);
    cJSON_AddNumberToObject(j, "naps", t->naps);
    return j;
}

void power_start(void)
{
    esp_reset_reason_t reset = esp_reset_reason();
    before = trail;
    before_ok = trail.magic == TRAIL_MAGIC && trail.crc == trail_crc(&trail);
    if (!before_ok) {
        memset(&trail, 0, sizeof trail);
        trail.magic = TRAIL_MAGIC;
        ESP_LOGI(TAG, "no sleep trail: the RTC memory is fresh (a power cycle, or the first boot of this program)");
    } else if (trail.step && trail.step != STEP_DONE) {
        ESP_LOGW(TAG, "the last sleep did not finish: it got to \"%s\" after %u s asleep, %u beats, %u light sleeps (%u rejected), woken by %s",
                 step_name(trail.step), (unsigned)trail.slept_s, (unsigned)trail.beats, (unsigned)trail.loops, (unsigned)trail.rejects, cause_name(trail.cause));
    } else {
        ESP_LOGI(TAG, "sleep trail: %u sleeps since the power cycle, the last %u s with %u beats, finished", (unsigned)trail.sleeps, (unsigned)trail.slept_s, (unsigned)trail.beats);
    }
    // The dead-battery count: a brownout within DOUBT_S of the boot before it adds one, a brownout after
    // a longer life starts at one, the guard's own timer wake keeps the count, any other boot (a
    // restart, an OTA, a crash, a power cycle, the RESET button) clears it.
    if (reset == ESP_RST_BROWNOUT) trail.browns = before_ok && before.up_s < DOUBT_S ? before.browns + 1 : 1;
    else if (reset != ESP_RST_DEEPSLEEP) trail.browns = 0;
    trail.up_s = 0;
    trail_save();
    if (reset == ESP_RST_BROWNOUT && trail.browns >= 2) {
        unsigned row = trail.browns - 2;
        uint32_t nap = NAP_S << (row > 5 ? 5 : row);
        if (nap > NAP_MAX_S) nap = NAP_MAX_S;
        trail.naps++;
        trail_save();
        ESP_LOGE(TAG, "brownout %u in a row, %u s into the boot before: the battery cannot carry a boot. Deep sleep %u s, then one try with the sound off",
                 (unsigned)trail.browns, (unsigned)before.up_s, (unsigned)nap);
        if (!board_dark()) ESP_LOGE(TAG, "the expander did not take the dark: the rail may stay on through the sleep");
        esp_sleep_enable_timer_wakeup((uint64_t)nap * 1000000);
        esp_deep_sleep_start();
    }
    doubt = reset == ESP_RST_DEEPSLEEP && trail.browns >= 2;
    if (doubt) ESP_LOGW(TAG, "the guard's try after %u brownouts in a row (%u naps): the amplifier stays off until %u s pass without one", (unsigned)trail.browns, (unsigned)trail.naps, DOUBT_S);
}

bool power_doubt(void) { return doubt; }

cJSON *power_json(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "sleeps", sleeps);
    cJSON_AddNumberToObject(j, "awake_s", power_awake_s());
    cJSON_AddBoolToObject(j, "no_sleep", no_sleep);
    cJSON_AddBoolToObject(j, "doubt", doubt);
    cJSON_AddItemToObject(j, "last", trail_json(&trail));
    if (before_ok) cJSON_AddItemToObject(j, "before_reset", trail_json(&before));
    return j;
}

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

// Everything but the chip off, in this order: the glass's lock held before the panel loses power,
// the stream and the ear stopped, the amplifier before the rail, WiFi before the timers it arms.
static void everything_off(void)
{
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
    storage_sleep(true);                           // the card loses the rail: unmounted first, mounted again at the wake (its first read after one timed out, 2026-11-08)
    board_rail(false);
}

// /power shutdown: off until RESET or a power cycle, the only honest "off" for a board with no
// switch. Deep sleep with no wake source at all. Before 0.5.5 it woke on GPIO 0, the pin the ROM
// reads as the download strap at a deep-sleep wake, with the rail left on (hunt 5.10).
void power_off(void)
{
    ESP_LOGI(TAG, "shutdown: everything off and deep sleep with no wake; RESET or a power cycle brings it back");
    everything_off();
    esp_deep_sleep_start();
}

static void go_to_sleep(void)
{
    sleeps++;
    ESP_LOGI(TAG, "going to sleep: the rail off, WiFi off; BOOT wakes it");
    everything_off();
    asleep = true;
    for (int i = 0; i < 150 && !gpio_get_level(BOARD_BOOT_BUTTON); i++) vTaskDelay(pdMS_TO_TICKS(20));   // the hold that asked for this sleep may still be down: a level wake would fire at once, a whole wake for nothing (scouts 2026-11-05)
    slept_from_us = esp_timer_get_time();
    trail.sleeps++;
    trail.loops = trail.rejects = trail.beats = 0;
    trail.cause = 0;
    trail_step(STEP_ASLEEP);
    gpio_wakeup_enable(BOARD_BOOT_BUTTON, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    uart_set_wakeup_threshold(UART_NUM_0, 3);      // a line typed on the console wakes it too (its first characters are lost)
    esp_sleep_enable_uart_wakeup(UART_NUM_0);
    esp_sleep_enable_timer_wakeup((uint64_t)BEAT_S * 1000000);   // the heartbeat: a few ms awake every half hour, counted in the trail, straight back to sleep
    esp_sleep_wakeup_cause_t why;
    for (;;) {
        esp_err_t err = esp_light_sleep_start();
        trail.loops++;
        if (err != ESP_OK) {
            // Rejected: a wake was already pending, or the sleep was refused. A rejected sleep has no
            // wake cause to read (IDF sleep_modes.c: the flag is set only on success), so before this
            // the loop went straight round: a hot spin, dark and silent, that no BOOT press could end,
            // because the press itself is what rejects the sleep (2026-11-08).
            trail.rejects++;
            trail_save();
            if (!gpio_get_level(BOARD_BOOT_BUTTON)) { why = ESP_SLEEP_WAKEUP_GPIO; break; }   // the press that rejected it is the wake
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        why = esp_sleep_get_wakeup_cause();
        if (why == ESP_SLEEP_WAKEUP_TIMER) { trail.beats++; trail_step(STEP_ASLEEP); continue; }
        if (why == ESP_SLEEP_WAKEUP_GPIO || why == ESP_SLEEP_WAKEUP_UART) break;
        trail_save();
    }
    trail.cause = why;
    trail_step(STEP_WOKE);
    gpio_wakeup_disable(BOARD_BOOT_BUTTON);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    ESP_LOGI(TAG, "awake: %s after %u s, %u beats, %u light sleeps (%u rejected)", cause_name(why), (unsigned)trail.slept_s, (unsigned)trail.beats, (unsigned)trail.loops, (unsigned)trail.rejects);
    woke_us = esp_timer_get_time();
    buttons_spend(1000);                           // BOOT, held until the picture showed, is not a long press (= sleep): it is the wake. A second's worth: the glass is back in ~0.9 s, and 2 s ate a real press for a second after it (scouts 2026-11-05)
    // One thing at a time: the rail, the glass, the amplifier, then the radio. All at once, the
    // supply sagged and the chip reset on brownout at the wake (reset "brownout" after Krem's
    // hold-to-wake, 2026-11-03: a picture for a second, then dark). The radio's first burst is
    // the biggest draw, so it comes last, half a second after the glass.
    if (!board_rail(true)) ESP_LOGE(TAG, "the rail did not come on at the wake: the expander did not take the write");   // tried thrice inside; silent before, with the log saying "awake" over a dark glass (scouts 2026-11-05)
    trail_step(STEP_RAIL);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (!board_glass_wake()) {
        ESP_LOGW(TAG, "the glass did not come back: once more");
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!board_glass_wake()) ESP_LOGE(TAG, "the glass did not come back");
    }
    trail_step(STEP_GLASS);
    asleep = false;
    ui_sleep(false);
    ui_toast("Waking: WiFi and her link take a few seconds", 5);   // a tap before they are back goes nowhere; say so (Krem's "unstable" wake, 2026-11-03)
    trail_step(STEP_UI);
    vTaskDelay(pdMS_TO_TICKS(150));
    audio_suspend(false);
    board_amplifier(!audio_muted());
    wake_pause(false);
    light_cue("idle");
    trail_step(STEP_SOUND);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_wifi_start();                              // net.c's handler joins again on STA_START
    esp_wifi_set_max_tx_power(44);                 // 11 dBm while it joins: the first bursts at full power are the wake's biggest draw; net.c restores full power once the IP lands
    trail_step(STEP_RADIO);
    net_sleep(false);
    storage_sleep(false);                          // the card, last: its init is a draw of its own
    trail_step(STEP_CARD);
    trail_step(STEP_DONE);
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
        if (no_sleep) ESP_LOGW(TAG, "the last reset was a brownout: sleep is off until a power cycle (the wake's load sagged the supply)");
        if (doubt) ui_toast("Low battery? It browned out twice. Sound off for two minutes while it proves itself", 8);
        else if (no_sleep) ui_toast("Brownout at the last wake: no sleep until power-cycled", 8);
    }
    trail.up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (trail.browns && trail.up_s >= DOUBT_S) {
        ESP_LOGI(TAG, "%u s up without a brownout: the battery doubt is over", DOUBT_S);
        trail.browns = 0;
        if (doubt) { doubt = false; board_amplifier(!audio_muted()); ui_toast("The battery carried it: sound is back", 4); }
    }
    trail_save();
    if (!asked && !no_sleep && settings.sleep_after_min && ui_ok() && !busy()
        && ui_idle_ms() > (int64_t)settings.sleep_after_min * 60000) asked = true;
    if (asked) {
        asked = false;
        if (busy()) { asked = true; return; }      // again next second
        if (!ui_ok()) { ESP_LOGW(TAG, "no glass up: sleep is only for a board with its glass"); return; }
        go_to_sleep();
    }
}
