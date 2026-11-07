// console.c - see console.h. Reads stdin (the USB serial port), no editing,
// no history: the other end is a script.
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_private/log_lock.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "lastwords.h"
#include "light.h"
#include "net.h"
#include "settings.h"
#include "stats.h"
#include "ui.h"
#include "camera.h"
#include "power.h"

static const char *TAG = "console";
#define CMD_MAX (CERT_MAX + 2048)     // a setup line carries the certificate

// An answer goes out whole, under the logger's own lock, or a log line from
// another task lands in the middle of it and the script cannot read it.
static void answer_text(const char *text)
{
    esp_log_impl_lock();
    printf(">> %s\n", text);
    fflush(stdout);
    esp_log_impl_unlock();
}

static void answer(cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    answer_text(text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(json);
}

static void take(cJSON *from, const char *name, char *into, size_t size)
{
    cJSON *v = cJSON_GetObjectItem(from, name);
    if (cJSON_IsString(v)) {
        strlcpy(into, v->valuestring, size);
    }
}

static void cmd_show(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", settings.name);
    cJSON_AddStringToObject(j, "wifi_ssid", settings.wifi_ssid);
    cJSON_AddStringToObject(j, "sapphire", settings.sapphire);
    cJSON_AddBoolToObject(j, "key", settings.key[0] != 0);
    cJSON_AddBoolToObject(j, "voice_key", settings.voice_key[0] != 0);
    cJSON_AddBoolToObject(j, "cert", settings.cert[0] != 0);
    cJSON_AddBoolToObject(j, "ready", settings_ready());
    cJSON_AddStringToObject(j, "ip", net_ip());
    cJSON_AddStringToObject(j, "firmware", PROJECT_VER);
    cJSON_AddStringToObject(j, "mac", board_mac());
    cJSON_AddNumberToObject(j, "free_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(j, "psram_kb", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    answer(j);
}

// The battery's pin as it is, whether or not it is a battery (0.7.5): the raw count, the
// pin's millivolts, the cell's, and how far 16 reads jumped. A floating pin shows here too.
static void cmd_battery(void)
{
    board_battery_t b;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "gpio", BOARD_BATTERY_GPIO);
    if (!board_battery_read(&b)) { cJSON_AddStringToObject(j, "error", "the ADC did not start"); answer(j); return; }
    cJSON_AddNumberToObject(j, "raw", b.raw);
    cJSON_AddNumberToObject(j, "pad_mv", b.pad_mv);
    cJSON_AddNumberToObject(j, "mv", b.mv);
    cJSON_AddNumberToObject(j, "spread_mv", b.spread_mv);
    cJSON_AddBoolToObject(j, "battery", b.battery);
    cJSON_AddNumberToObject(j, "percent", b.percent);
    answer(j);
}

static void cmd_i2c(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(j, "found");
    int n = board_i2c_scan(list);
    if (n < 0) { cJSON_Delete(j); answer_text("{\"error\": \"no I2C bus: the board did not come up\"}"); return; }
    answer(j);
}

static void cmd_scan(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(j, "networks");
    if (net_scan(list) < 0) {
        cJSON_Delete(j);
        answer_text("{\"error\": \"could not scan now; try again in a moment\"}");
        return;
    }
    answer(j);
}

static void cmd_keys(void)
{
    // Whoever holds the USB cable owns the board, so this hides nothing from
    // them. The setup script shows these once and never logs them.
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "key", settings.key);
    cJSON_AddStringToObject(j, "voice_key", settings.voice_key);
    answer(j);
}

static void cmd_setup(const char *json)
{
    cJSON *in = cJSON_Parse(json);
    if (!in) { ESP_LOGW(TAG, "setup: %u bytes did not parse as JSON", (unsigned)strlen(json)); answer_text("{\"error\": \"setup wants JSON\"}"); return; }
    static const char *FIELDS[] = {"name", "wifi_ssid", "wifi_password", "key", "voice_key", "sapphire", "cert"};
    char got[160] = "";
    for (size_t i = 0; i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        const cJSON *v = cJSON_GetObjectItem(in, FIELDS[i]);
        size_t n = strlen(got);
        snprintf(got + n, sizeof got - n, "%s%s=%s", i ? " " : "", FIELDS[i], !v ? "absent" : !cJSON_IsString(v) ? "not-text" : v->valuestring[0] ? "set" : "EMPTY");
    }
    ESP_LOGI(TAG, "setup: %u bytes, %s", (unsigned)strlen(json), got);      // the browser's log box shows this line
    take(in, "name", settings.name, sizeof settings.name);
    take(in, "wifi_ssid", settings.wifi_ssid, sizeof settings.wifi_ssid);
    take(in, "wifi_password", settings.wifi_password, sizeof settings.wifi_password);
    take(in, "key", settings.key, sizeof settings.key);
    take(in, "voice_key", settings.voice_key, sizeof settings.voice_key);
    take(in, "sapphire", settings.sapphire, sizeof settings.sapphire);
    take(in, "cert", settings.cert, sizeof settings.cert);
    cJSON_Delete(in);
    if (!settings.name[0]) strcpy(settings.name, "satellite");
    if (!settings_save()) { ESP_LOGW(TAG, "setup: flash refused the settings"); answer_text("{\"error\": \"flash refused the settings\"}"); return; }
    ESP_LOGI(TAG, "setup: saved, ready=%d, restarting", settings_ready());
    answer_text("{\"ok\": true, \"restarting\": true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void cmd_selftest(void)
{
    audio_selftest_t t;
    audio_selftest(&t);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "quiet_rms", t.quiet_rms);
    cJSON_AddNumberToObject(j, "tone_rms", t.tone_rms);
    cJSON_AddBoolToObject(j, "mic_hears_speaker", t.tone_rms > t.quiet_rms * 3 && t.tone_rms > 50);
    cJSON_AddBoolToObject(j, "codec_ok", t.codec_ok);
    cJSON_AddNumberToObject(j, "out_quiet", (int)t.out_quiet);          // the front end's output, quiet
    cJSON_AddNumberToObject(j, "out_tone", (int)t.out_tone);            // ... during the tone's second half: lower = the canceller eats more of the speaker
    cJSON_AddBoolToObject(j, "aec", audio_aec_enabled());
    cJSON_AddNumberToObject(j, "aec_delay_ms", audio_aec_delay_ms());
    cJSON *slots = cJSON_AddArrayToObject(j, "slots");     // every input slot during the tone
    for (int i = 0; i < 2; i++) cJSON_AddItemToArray(slots, cJSON_CreateNumber((int)t.slot_rms[i]));
    cJSON *quiet = cJSON_AddArrayToObject(j, "quiet_slots");
    for (int i = 0; i < 2; i++) cJSON_AddItemToArray(quiet, cJSON_CreateNumber((int)t.quiet_slots[i]));
    answer(j);
}

// screen invert 0|1 | flip 0|1 | bright 0..100: kept in flash, applied at once
static void cmd_screen(const char *rest)
{
    char what[16] = {0};
    int value = 0;
    if (sscanf(rest, "%15s %d", what, &value) != 2) { answer_text("{\"error\": \"screen invert|flip 0|1, or screen bright 0..100\"}"); return; }
    if (!strcmp(what, "invert")) settings.invert = value != 0;
    else if (!strcmp(what, "flip")) settings.flip = value != 0;
    else if (!strcmp(what, "mirror")) settings.mirror = value != 0;
    else if (!strcmp(what, "bright")) settings.brightness = value < 0 ? 0 : value > 100 ? 100 : value;
    else if (!strcmp(what, "touchdy")) settings.touch_dy = value < -40 ? -40 : value > 40 ? 40 : value;
    else { answer_text("{\"error\": \"screen invert|flip 0|1, or screen bright 0..100\"}"); return; }
    settings_save();
    ui_orient();
    board_backlight(settings.brightness);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "invert", settings.invert);
    cJSON_AddBoolToObject(j, "flip", settings.flip);
    cJSON_AddNumberToObject(j, "bright", settings.brightness);
    cJSON_AddBoolToObject(j, "glass", ui_ok());
    answer(j);
}

// aec | aec delay N | aec on|off | aec filter N | aec measure: the echo reference, the R&D knob (audio.c)
static void cmd_aec(const char *rest)
{
    char what[16] = {0};
    int value = 0;
    int n = sscanf(rest, "%15s %d", what, &value);
    if (n >= 1 && !strcmp(what, "delay") && n == 2) audio_aec_set_delay_ms(value);
    else if (n >= 1 && !strcmp(what, "on")) audio_aec_enable(true);
    else if (n >= 1 && !strcmp(what, "off")) audio_aec_enable(false);
    else if (n >= 1 && !strcmp(what, "filter") && n == 2) { settings.aec_filter = value < 1 ? 1 : value > 8 ? 8 : value; settings_save(); }
    else if (n >= 1 && !strcmp(what, "measure")) {
        int delay; float ratio;
        if (!audio_aec_measure(&delay, &ratio)) { answer_text("{\"error\": \"no sound is running, or the speaker is busy\"}"); return; }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "heard_after_ms", delay);
        cJSON_AddNumberToObject(j, "ratio", (int)ratio);
        cJSON_AddBoolToObject(j, "heard", delay >= 0);
        cJSON_AddNumberToObject(j, "delay_set_ms", audio_aec_delay_ms());
        if (delay >= 0) cJSON_AddNumberToObject(j, "suggest_ms", delay > 16 ? delay - 16 : 0);
        answer(j);
        return;
    }
    else if (n >= 1) { answer_text("{\"error\": \"aec | aec delay 0..500 | aec on|off | aec filter 1..8 (restart) | aec measure\"}"); return; }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "on", audio_aec_enabled());
    cJSON_AddNumberToObject(j, "delay_ms", audio_aec_delay_ms());
    cJSON_AddNumberToObject(j, "filter", settings.aec_filter);
    cJSON_AddNumberToObject(j, "mic_gain_db", audio_mic_gain());
    float rms; int speech;
    audio_stats(&rms, &speech);
    cJSON_AddNumberToObject(j, "out_rms", (int)rms);                 // what leaves the front end now
    cJSON_AddNumberToObject(j, "speech_ms_2s", speech);           // how much of the last 2 s the VAD called speech
    answer(j);
}

// touch: the chip's id registers, then 20 s of polling: every finger printed as TOUCH x y, with its INT line
static void cmd_touch(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "chip_id_a7", board_touch_reg(0xA7));
    cJSON_AddNumberToObject(j, "fw_a9", board_touch_reg(0xA9));
    cJSON_AddNumberToObject(j, "autosleep_fe", board_touch_reg(0xFE));
    cJSON_AddNumberToObject(j, "irq_ctl_fa", board_touch_reg(0xFA));
    cJSON_AddBoolToObject(j, "int_low", board_touch_int());
    int errors, resets, held;
    board_i2c_counts(&errors, &resets, &held);
    cJSON_AddNumberToObject(j, "read_errors", errors);
    cJSON_AddNumberToObject(j, "rests", resets);
    cJSON_AddStringToObject(j, "then", "60 s: every finger-down prints TOUCH x y (LVGL's own reader)");
    answer(j);
    ui_touch_echo(60);
}

// snap [WxH]: a picture, timed; the bytes stay on the board
static void cmd_snap(const char *rest)
{
    int w = 800, h = 600;
    sscanf(rest, "%dx%d", &w, &h);
    snap_t s; char why[96] = "";
    int64_t t0 = esp_timer_get_time();
    bool ok = camera_snap(w, h, strstr(rest, "flash") != NULL, &s, why, sizeof why);
    cJSON *j = cJSON_CreateObject();
    if (ok) {
        cJSON_AddNumberToObject(j, "w", s.w); cJSON_AddNumberToObject(j, "h", s.h);
        cJSON_AddNumberToObject(j, "bytes", s.len);
        cJSON_AddNumberToObject(j, "encode_ms", camera_last_encode_ms());
        cJSON_AddNumberToObject(j, "total_ms", (int)((esp_timer_get_time() - t0) / 1000));
        camera_snap_done(&s);
    } else cJSON_AddStringToObject(j, "error", why);
    answer(j);
}

// touch raw: registers 0x00..0x07 of the touch chip every 50 ms for 15 s, printed when they change
static void cmd_touch_raw(void)
{
    answer_text("{\"ok\": true, \"then\": \"15 s: RAW 00 01 02 03 04 05 06 07 printed on change\"}");
    int last[8] = {-2, -2, -2, -2, -2, -2, -2, -2};
    int64_t end = esp_timer_get_time() + 15000000;
    int lines = 0;
    while (esp_timer_get_time() < end && lines < 60) {
        int now[8];
        bool changed = false;
        for (int r = 0; r < 8; r++) { now[r] = board_touch_reg(r); if (now[r] != last[r]) changed = true; }
        if (changed) {
            lines++;
            printf("RAW %02x %02x %02x %02x %02x %02x %02x %02x int=%d\n", now[0] & 255, now[1] & 255, now[2] & 255, now[3] & 255, now[4] & 255, now[5] & 255, now[6] & 255, now[7] & 255, board_touch_int());
            fflush(stdout);
            memcpy(last, now, sizeof now);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    answer_text("{\"done\": true}");
}

static void cmd_heap(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "internal_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(j, "internal_largest_kb", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(j, "dma_largest_kb", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024);
    cJSON_AddNumberToObject(j, "internal_min_ever_kb", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(j, "psram_kb", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    answer(j);
}

static void console_task(void *arg)
{
    char *line = malloc(CMD_MAX);
    if (!line) vTaskDelete(NULL);
    for (;;) {
        if (!fgets(line, CMD_MAX, stdin)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (n > 200) ESP_LOGI(TAG, "a line of %u bytes: %.20s...", (unsigned)n, line);
        if (!strcmp(line, "show")) cmd_show();
        else if (!strcmp(line, "keys")) cmd_keys();
        else if (!strcmp(line, "scan")) cmd_scan();
        else if (!strncmp(line, "setup ", 6)) cmd_setup(line + 6);
        else if (!strcmp(line, "selftest")) cmd_selftest();
        else if (!strcmp(line, "tasks")) answer(stats_tasks());
        else if (!strcmp(line, "i2c")) cmd_i2c();
        else if (!strcmp(line, "battery")) cmd_battery();
        else if (!strcmp(line, "heap")) cmd_heap();
        else if (!strcmp(line, "touch raw")) cmd_touch_raw();
        else if (!strcmp(line, "touch")) cmd_touch();
        else if (!strcmp(line, "aec")) cmd_aec("");
        else if (!strncmp(line, "aec ", 4)) cmd_aec(line + 4);
        else if (!strcmp(line, "snap")) cmd_snap("");
        else if (!strncmp(line, "snap ", 5)) cmd_snap(line + 5);
        else if (!strcmp(line, "sleep")) { answer_text("{\"ok\": true, \"wake\": \"press BOOT, or type a line\"}"); power_sleep_soon(); }
        else if (!strncmp(line, "rail ", 5)) { board_rail(line[5] != '0'); answer_text(board_rail_on() ? "{\"rail\": true}" : "{\"rail\": false}"); }
        else if (!strncmp(line, "fill ", 5)) { board_fill_light(line[5] != '0'); answer_text(board_fill_light_on() ? "{\"fill\": true}" : "{\"fill\": false}"); }
        else if (!strncmp(line, "app ", 4)) answer_text(ui_show_app(atoi(line + 4)) ? "{\"ok\": true}" : "{\"error\": \"0, 1 or 2, with a glass up\"}");
        else if (!strcmp(line, "where")) answer(lastwords_where());
        // the two ways down, on purpose, to see that each is caught and told (0.5.2)
        else if (!strcmp(line, "fault glass")) { answer_text("{\"ok\": true, \"expect\": \"503 from the screen, a restart in 10 s, then lastwords\"}"); ui_test_stuck(); }
        else if (!strcmp(line, "fault crash")) { answer_text("{\"ok\": true, \"expect\": \"a restart now, then lastwords\"}"); vTaskDelay(pdMS_TO_TICKS(200)); abort(); }
        else if (!strcmp(line, "lastwords forget")) { lastwords_forget(); answer_text("{\"ok\": true}"); }
        else if (!strcmp(line, "lastwords")) { cJSON *w = lastwords_json(true); if (w) answer(w); else answer_text("{\"none\": true}"); }
        else if (!strcmp(line, "test scene")) {            // a full background made here: what a chat's scene costs, with no network in it
            uint8_t *px = ui_image_begin(UI_SCENE, BOARD_W, BOARD_H);
            for (int y = 0; px && y < BOARD_H; y++) for (int x = 0; x < BOARD_W; x++) {
                uint16_t c = (uint16_t)((x * 31 / BOARD_W) << 11 | ((x ^ y) & 63) << 5 | (y * 31 / BOARD_H));
                px[(y * BOARD_W + x) * 2] = c >> 8;       // big-endian, as the wire has it
                px[(y * BOARD_W + x) * 2 + 1] = c & 255;
            }
            answer_text(px && ui_image_show(UI_SCENE, BOARD_W, BOARD_H, 0, "test") ? "{\"ok\": true}" : "{\"error\": \"the glass did not take it\"}");
        }
        else if (!strncmp(line, "test chat ", 10)) answer_text(ui_chat(line + 10, "test-model", 0x2a6fdb) ? "{\"ok\": true}" : "{\"error\": \"the glass did not answer\"}");
        else if (!strcmp(line, "test scene off")) answer_text(ui_image_drop(UI_SCENE) ? "{\"ok\": true}" : "{\"error\": \"the glass did not answer\"}");
        else if (!strcmp(line, "test")) { answer_text(ui_ok() ? "{\"ok\": true}" : "{\"error\": \"no glass is up\"}"); ui_test_card(); }
        else if (!strncmp(line, "screen ", 7)) cmd_screen(line + 7);
        else if (!strncmp(line, "light ", 6)) {
            int r = 0, g = 0, b = 0;
            sscanf(line + 6, "%d %d %d", &r, &g, &b);
            answer_text("{\"ok\": true}");
            light_show(r & 255, g & 255, b & 255, 6);
        }
        else if (!strcmp(line, "restart")) { answer_text("{\"ok\": true}"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
        else if (!strcmp(line, "erase")) { settings_erase(); answer_text("{\"ok\": true, \"erased\": true}"); }
        else answer_text("{\"error\": \"commands: show, keys, scan, setup {json}, selftest, touch, aec [delay N|on|off|filter N|measure], snap [WxH], sleep, rail 0|1, fill 0|1, app 0|1|2, tasks, where, lastwords [forget], fault glass|crash, i2c, heap, test, test scene [off], screen invert|flip|bright N, light r g b, restart, erase\"}");
    }
}

void console_start(void)
{
    // stdin blocks in fgets only with the USB serial driver installed (main.c)
    xTaskCreate(console_task, "console", 6144, NULL, 3, NULL);   // scan brings the radio up from here
    ESP_LOGI(TAG, "serial console ready");
}
