// console.c - see console.h.
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

#include "board.h"
#include "light.h"
#include "net.h"
#include "settings.h"
#include "status.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "console";
#define CMD_MAX (CERT_MAX + 2048)

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
    cJSON_AddStringToObject(j, "board", BOARD_NAME);
    cJSON_AddBoolToObject(j, "flip", board()->flip);
    cJSON_AddNumberToObject(j, "brightness", settings.brightness);
    storage_state_t card;
    cJSON_AddStringToObject(j, "status", status_word(status_now(&card)));
    cJSON_AddBoolToObject(j, "card", card.mounted);
    cJSON_AddNumberToObject(j, "backups", card.count);
    cJSON_AddNumberToObject(j, "damaged", card.damaged);
    cJSON_AddNumberToObject(j, "unchecked", card.unchecked);
    cJSON_AddNumberToObject(j, "free_kb", esp_get_free_heap_size() / 1024);
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
    if (!settings.name[0]) strcpy(settings.name, "dongle");
    if (!settings_save()) { answer_text("{\"error\": \"flash refused the settings\"}"); return; }
    answer_text("{\"ok\": true, \"restarting\": true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void cmd_screen(const char *rest)
{
    char what[16] = {0};
    int value = -1;
    if (sscanf(rest, "%15s %d", what, &value) < 2) { answer_text("{\"error\": \"screen flip 0|1, bright 10..100\"}"); return; }
    if (!strcmp(what, "bright")) {
        settings.brightness = value < 10 ? 10 : value > 100 ? 100 : value;
        settings_save();
        board_backlight(settings.brightness);
    }
    else if (!strcmp(what, "flip")) {
        board_set_flip(value != 0);
        answer_text("{\"ok\": true, \"restarting\": true}");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    else { answer_text("{\"error\": \"screen flip|bright\"}"); return; }
    answer_text("{\"ok\": true}");
}

// card [MB] [keep]: a sealed-looking file of that size through the very path a
// backup takes (room taken first, written, read back, its hash kept), timed,
// then removed. For proving a card and this board's wiring without Sapphire.
static void cmd_card(const char *rest)
{
    int mb = 4;
    char keep[8] = "";
    sscanf(rest, "%d %7s", &mb, keep);
    if (mb < 1 || mb > 512) mb = 4;
    const char *name = "sapphire_selftest.sapphirebak";
    uint64_t size = (uint64_t)mb << 20, left = size;
    int64_t t0 = esp_timer_get_time();
    storage_write_t *w = storage_write_begin(name, size);
    if (!w) { answer_text("{\"error\": \"the card is not there, or busy\"}"); return; }
    uint8_t *buf = malloc(4096);
    bool ok = buf != NULL;
    for (int i = 0; ok && i < 4096; i++) buf[i] = (uint8_t)(i * 31 + 7);
    if (ok) memcpy(buf, "SAPPHIREBAK\x01", 12);
    while (ok && left) {
        size_t n = left < 4096 ? left : 4096;
        ok = storage_write_feed(w, buf, n);
        left -= n;
    }
    free(buf);
    char sha[65] = "";
    int64_t t1 = esp_timer_get_time();
    if (ok) ok = storage_write_finish(w, sha, NULL);
    else storage_write_drop(w);
    int64_t t2 = esp_timer_get_time();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", ok);
    cJSON_AddNumberToObject(j, "mb", mb);
    cJSON_AddNumberToObject(j, "write_ms", (double)((t1 - t0) / 1000));
    cJSON_AddNumberToObject(j, "finish_ms", (double)((t2 - t1) / 1000));       // the sync, the read back, the hash file, the rename
    cJSON_AddStringToObject(j, "sha256", sha);
    cJSON_AddNumberToObject(j, "hash_kept", storage_checked(name));           // 1 on a board that keeps hashes
    if (strcmp(keep, "keep")) storage_remove(name);
    answer(j);
}

// flipcard: one byte of the kept test file (`card N keep`) turned over, so the
// check at the next start has something to find. It can touch no other file.
static void cmd_flipcard(void)
{
    char path[160];
    FILE *f = storage_path("sapphire_selftest.sapphirebak", path, sizeof path) ? fopen(path, "r+b") : NULL;
    if (!f) { answer_text("{\"error\": \"no test file: card 4 keep first\"}"); return; }
    int c = (fseek(f, 100000, SEEK_SET), fgetc(f));
    bool ok = c != EOF && !fseek(f, 100000, SEEK_SET) && fputc(c ^ 0xFF, f) != EOF;
    fclose(f);
    answer_text(ok ? "{\"ok\": true, \"flipped\": 100000}" : "{\"error\": \"could not write\"}");
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
        else if (!strcmp(line, "flipcard")) cmd_flipcard();
        else if (!strcmp(line, "card") || !strncmp(line, "card ", 5)) cmd_card(line + 4);
        else if (!strcmp(line, "test")) { answer_text("{\"ok\": true, \"card\": true}"); ui_test_card(); }
        else if (!strncmp(line, "screen ", 7)) cmd_screen(line + 7);
        else if (!strncmp(line, "light ", 6)) {
            int r = 0, g = 0, b = 0;
            sscanf(line + 6, "%d %d %d", &r, &g, &b);
            answer_text("{\"ok\": true}");
            light_show(r & 255, g & 255, b & 255, 6);
        }
        else if (!strcmp(line, "restart")) { answer_text("{\"ok\": true}"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
        else if (!strcmp(line, "erase")) { settings_erase(); answer_text("{\"ok\": true, \"erased\": true}"); }
        else answer_text("{\"error\": \"commands: show, keys, scan, setup {json}, test, card [MB], screen flip|bright N, light r g b, restart, erase\"}");
    }
}

void console_start(void)
{
    xTaskCreate(console_task, "console", 8192, NULL, 3, NULL);   // scan brings the radio up from here
    ESP_LOGI(TAG, "serial console ready");
}
