// console.c - setup over the USB serial line (see console.h). Speaks the same
// protocol as the other Sapphire boards so the Devices page flashes AND
// provisions the Cardputer through the normal Add-Device flow: a line in, a
// ">> {json}" answer out. P1 implements the provisioning subset (no audio or
// glass hardware yet): show, scan, setup {json}, keys, restart, erase.
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/log_lock.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "net.h"
#include "settings.h"
#include "board.h"
#include "link.h"
#include "screen.h"
#include "storage.h"
#include "ir.h"

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
    if (cJSON_IsString(v)) strlcpy(into, v->valuestring, size);
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
    cJSON_AddNumberToObject(j, "psram_kb", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);   // 0: no PSRAM
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
    // Whoever holds the USB cable owns the board, so this hides nothing from them.
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "key", settings.key);
    cJSON_AddStringToObject(j, "voice_key", settings.voice_key);
    answer(j);
}

static void cmd_setup(const char *json)
{
    cJSON *in = cJSON_Parse(json);
    if (!in) { answer_text("{\"error\": \"setup wants JSON\"}"); return; }
    take(in, "name", settings.name, sizeof settings.name);
    take(in, "wifi_ssid", settings.wifi_ssid, sizeof settings.wifi_ssid);
    take(in, "wifi_password", settings.wifi_password, sizeof settings.wifi_password);
    take(in, "key", settings.key, sizeof settings.key);
    take(in, "voice_key", settings.voice_key, sizeof settings.voice_key);
    take(in, "sapphire", settings.sapphire, sizeof settings.sapphire);
    take(in, "cert", settings.cert, sizeof settings.cert);
    cJSON_Delete(in);
    if (!settings.name[0]) strcpy(settings.name, "satellite");
    if (!settings_save()) { answer_text("{\"error\": \"flash refused the settings\"}"); return; }
    answer_text("{\"ok\": true, \"restarting\": true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void cmd_heap(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "internal_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(j, "internal_largest_kb", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(j, "min_ever_kb", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    answer(j);
}

// Capture ~1 s of mic and report what it looks like. A working ES8311 reads a
// small varying RMS with many nonzero samples even in a quiet room; the #18621
// regression shows as rms~0 / nonzero 0 / a stuck constant value in first8.
static void cmd_mic(void)
{
    const int n = 16000;                 // 1 s at 16 kHz
    int16_t *buf = malloc((size_t)n * sizeof(int16_t));
    if (!buf) { answer_text("{\"error\": \"no RAM for a 1 s capture\"}"); return; }
    int got = board_mic_read(buf, n);
    if (got < 0) {
        char err[96];
        snprintf(err, sizeof err, "{\"error\": \"mic read failed: %s\"}", board_trouble()[0] ? board_trouble() : "no codec");
        answer_text(err);
        free(buf);
        return;
    }
    long long sumsq = 0;
    int16_t mn = 32767, mx = -32768;
    int nonzero = 0;
    for (int i = 0; i < got; i++) {
        int v = buf[i];
        sumsq += (long long)v * v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        if (v) nonzero++;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "samples", got);
    cJSON_AddNumberToObject(j, "rms", got ? sqrt((double)sumsq / got) : 0);
    cJSON_AddNumberToObject(j, "min", mn);
    cJSON_AddNumberToObject(j, "max", mx);
    cJSON_AddNumberToObject(j, "nonzero", nonzero);
    cJSON *first = cJSON_AddArrayToObject(j, "first8");
    for (int i = 0; i < 8 && i < got; i++) cJSON_AddItemToArray(first, cJSON_CreateNumber(buf[i]));
    answer(j);
    free(buf);
}

// Capture `seconds` of mic and push it to Sapphire's voice door — the whole
// voice-up loop, triggerable without the PTT key. Reports what she heard.
static void cmd_test_voice(int seconds)
{
    if (seconds < 1) seconds = 2;
    if (seconds > 5) seconds = 5;                 // cap: 5 s = 160 KB in internal RAM
    int n = seconds * AUDIO_RATE;
    int16_t *buf = malloc((size_t)n * sizeof(int16_t));
    if (!buf) { answer_text("{\"error\": \"no RAM for the capture\"}"); return; }
    int got = board_mic_read(buf, n);
    if (got < 0) { answer_text("{\"error\": \"mic read failed\"}"); free(buf); return; }
    char heard[256] = {0};
    bool ok = link_send_voice(buf, got, heard, sizeof heard);
    free(buf);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "sent", ok);
    cJSON_AddNumberToObject(j, "seconds", seconds);
    cJSON_AddStringToObject(j, "heard", heard);
    answer(j);
}

// Dump raw TCA8418 key numbers for `seconds` so we can map a physical key:
// press Ctrl, read the code it prints. One line per press and release.
static void cmd_kbd(int seconds)
{
    if (seconds < 1) seconds = 8;
    if (seconds > 30) seconds = 30;
    if (!board_kbd_init()) { answer_text("{\"error\": \"TCA8418 keyboard not responding\"}"); return; }
    board_kbd_hold(true);                        // stop the PTT task racing us for FIFO events
    answer_text("{\"kbd\": \"press keys now\"}");
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(seconds * 1000);
    while ((int)(until - xTaskGetTickCount()) > 0) {
        int key; bool down;
        while (board_kbd_read(&key, &down)) {
            char line[64];
            snprintf(line, sizeof line, "{\"key\": %d, \"down\": %s}", key, down ? "true" : "false");
            answer_text(line);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    board_kbd_hold(false);
    answer_text("{\"kbd\": \"done\"}");
}

// Exercise the streaming (chunked) voice path without the Ctrl key: capture
// `seconds` and feed it through link_voice_begin/write/end, exactly as PTT does.
// Lets us prove the chunk framing parses server-side before testing on-metal.
static void cmd_test_stream(int seconds)
{
    static int16_t buf[1600];                     // 100 ms; static, off the console stack
    if (seconds < 1) seconds = 2;
    if (seconds > 20) seconds = 20;
    if (!link_voice_begin()) { answer_text("{\"error\": \"could not open the voice stream\"}"); return; }
    int ms = 0;
    bool ok = true;
    while (ok && ms < seconds * 1000) {
        int got = board_mic_read(buf, 1600);
        if (got <= 0) break;
        ok = link_voice_write(buf, got);
        ms += got * 1000 / AUDIO_RATE;
    }
    char heard[256] = {0};
    bool sent = link_voice_end(heard, sizeof heard);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "sent", sent);
    cJSON_AddNumberToObject(j, "ms", ms);
    cJSON_AddStringToObject(j, "heard", heard);
    answer(j);
}

// Live screen geometry tuning, so the panel's orientation/offset is found on
// glass without reflashing: `screen test`, `screen home`, `screen cfg ...`.
static void cmd_screen(const char *args)
{
    while (*args == ' ') args++;
    if (!strncmp(args, "test", 4)) { screen_test(); answer_text("{\"ok\": true, \"screen\": \"test\"}"); }
    else if (!strncmp(args, "cal", 3)) { screen_cal(); answer_text("{\"ok\": true, \"screen\": \"cal\"}"); }
    else if (!strncmp(args, "ruler", 5)) { screen_ruler(); answer_text("{\"ok\": true, \"screen\": \"ruler\"}"); }
    else if (!strncmp(args, "home", 4)) { screen_home(); answer_text("{\"ok\": true, \"screen\": \"home\"}"); }
    else if (!strncmp(args, "cfg", 3)) {
        int fx = 0, fy = 0, gx = 52, gy = 40;
        if (sscanf(args + 3, "%d %d %d %d", &fx, &fy, &gx, &gy) == 4) {
            screen_config(fx, fy, gx, gy);
            char msg[112];
            snprintf(msg, sizeof msg, "{\"ok\": true, \"flip\": [%d, %d], \"gap\": [%d, %d]}", fx, fy, gx, gy);
            answer_text(msg);
        } else answer_text("{\"error\": \"screen cfg <flipx> <flipy> <gapx> <gapy>\"}");
    }
    else answer_text("{\"error\": \"screen test | home | cfg <flipx> <flipy> <gapx> <gapy>\"}");
}

// Play a 1 s tone on the ES8311 speaker: proves the DAC + amp path works before
// we rely on Sapphire's TTS. Fades in/out so there is no click.
static void cmd_test_tone(void)
{
    const int total = AUDIO_RATE;      // 1 s
    int16_t buf[320];
    for (int at = 0; at < total; at += 320) {
        int n = total - at < 320 ? total - at : 320;
        for (int i = 0; i < n; i++) {
            float edge = fminf(1.0f, fminf((at + i) / 400.0f, (total - at - i) / 400.0f));
            buf[i] = (int16_t)(9000 * edge * sinf(2 * M_PI * 880.0f * (at + i) / AUDIO_RATE));
        }
        if (board_speaker_write(buf, n) < 0) { answer_text("{\"error\": \"speaker write failed\"}"); return; }
    }
    answer_text("{\"ok\": true, \"tone\": 880}");
}

// The microSD card: mount state and how much room. Retries the mount, so a card
// put in after boot shows up on the next `sd`.
static void cmd_sd(void)
{
    cJSON *j = cJSON_CreateObject();
    bool ok = storage_ok() || storage_start();
    cJSON_AddBoolToObject(j, "mounted", ok);
    if (ok) {
        uint64_t total = 0, freeb = 0;
        storage_room(&freeb, &total);
        cJSON_AddStringToObject(j, "card", storage_card_name());
        cJSON_AddNumberToObject(j, "total_mb", (double)(total >> 20));
        cJSON_AddNumberToObject(j, "free_mb", (double)(freeb >> 20));
    }
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
        if (!strcmp(line, "show")) cmd_show();
        else if (!strcmp(line, "keys")) cmd_keys();
        else if (!strcmp(line, "scan")) cmd_scan();
        else if (!strncmp(line, "setup ", 6)) cmd_setup(line + 6);
        else if (!strcmp(line, "restart")) { answer_text("{\"ok\": true}"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
        else if (!strcmp(line, "erase")) { settings_erase(); answer_text("{\"ok\": true, \"erased\": true}"); }
        else if (!strcmp(line, "mic")) cmd_mic();
        else if (!strcmp(line, "heap")) cmd_heap();
        else if (!strcmp(line, "sd")) cmd_sd();
        else if (!strncmp(line, "ir ", 3)) {
            unsigned addr = 0, cmd = 0;    // `ir <addr_hex> <cmd_hex>` - one NEC frame (LG power = ir 04 08, TCL = ir 57e3 e8)
            if (sscanf(line + 3, "%x %x", &addr, &cmd) == 2) {
                ir_send_nec((uint16_t)addr, (uint8_t)cmd);
                char m[56]; snprintf(m, sizeof m, "{\"ok\": true, \"nec\": \"%x %02x\"}", addr & 0xffff, cmd & 0xff);
                answer_text(m);
            } else answer_text("{\"error\": \"ir <addr_hex> <cmd_hex>\"}");
        }
        else if (!strncmp(line, "vol", 3)) { int v = atoi(line + 3); board_speaker_vol(v); char m[40]; snprintf(m, sizeof m, "{\"ok\": true, \"vol\": %d}", v); answer_text(m); }
        else if (!strcmp(line, "test tone")) cmd_test_tone();
        else if (!strncmp(line, "test stream", 11)) cmd_test_stream(atoi(line + 11));
        else if (!strncmp(line, "test voice", 10)) cmd_test_voice(atoi(line + 10));
        else if (!strncmp(line, "kbd", 3)) cmd_kbd(atoi(line + 3));
        else if (!strncmp(line, "screen", 6)) cmd_screen(line + 6);
        else answer_text("{\"error\": \"commands: show, keys, scan, setup {json}, mic, heap, sd, kbd [sec], test voice [sec], test stream [sec], screen test|home|cfg, restart, erase\"}");
    }
}

void console_start(void)
{
    // stdin blocks in fgets only with the USB serial driver installed (main.c)
    xTaskCreate(console_task, "console", 6144, NULL, 3, NULL);   // scan brings the radio up from here
    ESP_LOGI(TAG, "USB console ready");
}
