// console.c - see console.h.
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_private/log_lock.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "light.h"
#include "net.h"
#include "settings.h"
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

static const char *panel_word(panel_t p) { return p == PANEL_ILI9341 ? "ili9341" : p == PANEL_ST7789 ? "st7789" : p == PANEL_ST7796 ? "st7796" : ""; }

static void cmd_show(void)
{
    board_t *b = board();
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
    cJSON_AddStringToObject(j, "panel", board_panel_name());
    cJSON_AddStringToObject(j, "panel_set", panel_word(b->panel_set));
    char id[16];
    snprintf(id, sizeof id, "%02x %02x %02x %02x", b->id[0], b->id[1], b->id[2], b->id[3]);
    cJSON_AddStringToObject(j, "panel_id", id);
    cJSON_AddBoolToObject(j, "touch", b->touch);
    cJSON_AddStringToObject(j, "touch_words", board_touch_words());
    cJSON_AddNumberToObject(j, "w", board_w());
    cJSON_AddNumberToObject(j, "h", board_h());
    cJSON_AddBoolToObject(j, "invert", b->invert);
    cJSON_AddBoolToObject(j, "bgr", b->bgr);
    cJSON_AddNumberToObject(j, "rotation", b->rotation);
    cJSON_AddBoolToObject(j, "mirror", b->mirror);
    cJSON_AddNumberToObject(j, "brightness", settings.brightness);
    cJSON_AddNumberToObject(j, "font", settings.font);
    cJSON_AddStringToObject(j, "home", ui_home_name());
    cJSON_AddNumberToObject(j, "light_raw", board_light_raw());
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
    if (!settings.name[0]) strcpy(settings.name, "pocket");
    if (!settings_save()) { answer_text("{\"error\": \"flash refused the settings\"}"); return; }
    answer_text("{\"ok\": true, \"restarting\": true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void restart_said(void)
{
    answer_text("{\"ok\": true, \"restarting\": true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void cmd_screen(const char *rest)
{
    char what[16] = {0}, word[16] = {0};
    if (sscanf(rest, "%15s %15s", what, word) < 2) {
        answer_text("{\"error\": \"screen invert|mirror|bgr 0|1, rotate 0|90|180|270, bright 10..100, font 14..24, panel auto|ili9341|st7789|st7796, home chat|backups|status|board|actions\"}");
        return;
    }
    int value = atoi(word);
    if (!strcmp(what, "invert") || !strcmp(what, "mirror")) {
        if (ui_up()) lvgl_port_lock(0);        // the panel takes a command from one task at a time: never under LVGL's flush
        if (what[0] == 'i') board_set_invert(value != 0);
        else board_set_mirror(value != 0);
        if (ui_up()) { lvgl_port_unlock(); if (what[0] != 'i') ui_repaint(); }
    }
    else if (!strcmp(what, "rotate")) {
        if (value != 0 && value != 90 && value != 180 && value != 270) { answer_text("{\"error\": \"rotate 0, 90, 180 or 270\"}"); return; }
        board_set_rotation(value);
        restart_said();                        // the screen is built at a size
        return;
    }
    else if (!strcmp(what, "bgr")) { board_set_bgr(value != 0); restart_said(); return; }
    else if (!strcmp(what, "home")) {
        if (!ui_set_home(word)) { answer_text("{\"error\": \"home chat, backups, status, board or actions\"}"); return; }
        ui_show_page((page_t)settings.home);
    }
    else if (!strcmp(what, "bright")) {
        settings.brightness = value < 10 ? 10 : value > 100 ? 100 : value;
        settings_save();
        board_backlight(settings.brightness);
    }
    else if (!strcmp(what, "font")) {
        if (value != 14 && value != 16 && value != 18 && value != 20 && value != 22 && value != 24) { answer_text("{\"error\": \"font 14, 16, 18, 20, 22 or 24\"}"); return; }
        settings.font = value;
        settings_save();
        restart_said();
        return;
    }
    else if (!strcmp(what, "panel")) {
        panel_t p = !strcmp(word, "ili9341") ? PANEL_ILI9341 : !strcmp(word, "st7789") ? PANEL_ST7789 : !strcmp(word, "st7796") ? PANEL_ST7796 : !strcmp(word, "auto") ? PANEL_AUTO : -1;
        if ((int)p < 0) { answer_text("{\"error\": \"panel auto, ili9341, st7789 or st7796\"}"); return; }
        board_set_panel(p);
        restart_said();
        return;
    }
    else { answer_text("{\"error\": \"screen invert|mirror|bgr|rotate|bright|font|panel|home\"}"); return; }
    answer_text("{\"ok\": true}");
}

// i2c <sda> <scl> [rst]: who answers on those two pins. For a sensor on the
// CN1 connector (22 and 27 are free on this board). With rst, that pin is
// pulled low then high first, so a chip held in reset gets to answer.
static void cmd_i2c_scan(const char *rest)
{
    int sda = -1, scl = -1, rst = -1;
    if (sscanf(rest, "%d %d %d", &sda, &scl, &rst) < 2 || sda < 0 || scl < 0) { answer_text("{\"error\": \"i2c <sda> <scl> [rst]\"}"); return; }
    // every other GPIO is the glass, the touch, the card, the LED, the amplifier, the flash or this very UART
    static const int FREE[] = BOARD_FREE_PINS;
    bool free_sda = false, free_scl = false, free_rst = rst < 0;
    for (size_t i = 0; i < sizeof FREE / sizeof FREE[0]; i++) { free_sda |= sda == FREE[i]; free_scl |= scl == FREE[i]; free_rst |= rst == FREE[i]; }
    if (!free_sda || !free_scl || !free_rst || sda == scl || sda == rst || scl == rst) {
        char t[96];
        snprintf(t, sizeof t, "{\"error\": \"i2c: only GPIO %d and %d are free on this board\"}", FREE[0], FREE[1]);
        answer_text(t);
        return;
    }
    if (rst >= 0) {
        gpio_config_t g = {.pin_bit_mask = 1ULL << rst, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&g);
        gpio_set_level(rst, 0); vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(rst, 1); vTaskDelay(pdMS_TO_TICKS(100));
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1, .sda_io_num = sda, .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) { char t[96]; snprintf(t, sizeof t, "{\"error\": \"bus on %d/%d: %s\"}", sda, scl, esp_err_to_name(err)); answer_text(t); return; }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "sda", sda);
    cJSON_AddNumberToObject(j, "scl", scl);
    cJSON *found = cJSON_AddArrayToObject(j, "found");
    for (int a = 0x08; a < 0x78; a++)
        if (i2c_master_probe(bus, a, 50) == ESP_OK) cJSON_AddItemToArray(found, cJSON_CreateNumber(a));
    i2c_del_master_bus(bus);
    answer(j);
}

// touch: the chip read raw for 10 seconds, four times a second, touched or not:
// RAW irq z1 z2 x y touched. irq is the PENIRQ pin (0 while touched), z1 + 4095 - z2 the pressure
static void cmd_touch(void)
{
    if (!ui_up()) { answer_text("{\"error\": \"the screen is not up yet\"}"); return; }
    answer_text("{\"ok\": true, \"seconds\": 10}");
    int seen = 0;
    for (int i = 0; i < 40; i++) {
        int x = -1, y = -1, z = 0, sx = -1, sy = -1;
        lvgl_port_lock(0);                     // LVGL reads the same chip on its own task: one at a time
        bool touched = board_touch_raw(&x, &y, &z);
        if (touched) board_touch(&sx, &sy);
        lvgl_port_unlock();
        printf("RAW x %d y %d z %d touched %d screen %d %d\n", x, y, z, touched, sx, sy);
        fflush(stdout);
        seen += touched;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    char t[64];
    snprintf(t, sizeof t, "{\"ok\": true, \"touches\": %d}", seen);
    answer_text(t);
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
        else if (!strcmp(line, "test")) { answer_text("{\"ok\": true, \"card\": true}"); ui_test_card(); }
        else if (!strncmp(line, "screen ", 7)) cmd_screen(line + 7);
        else if (!strncmp(line, "i2c ", 4)) cmd_i2c_scan(line + 4);
        else if (!strcmp(line, "touch")) cmd_touch();
        else if (!strcmp(line, "touch cal")) { answer_text("{\"ok\": true, \"calibrating\": true}"); if (!ui_up()) answer_text("{\"error\": \"the screen is not up yet\"}"); else ui_calibrate(); }
        else if (!strncmp(line, "light ", 6)) {
            int r = 0, g = 0, b = 0;
            sscanf(line + 6, "%d %d %d", &r, &g, &b);
            answer_text("{\"ok\": true}");
            light_show(r & 255, g & 255, b & 255, 6);
        }
        else if (!strcmp(line, "restart")) {
            if (storage_busy()) answer_text("{\"error\": \"a backup is arriving: not now\"}");
            else { answer_text("{\"ok\": true}"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
        }
        else if (!strcmp(line, "erase")) { settings_erase(); answer_text("{\"ok\": true, \"erased\": true}"); }
        else answer_text("{\"error\": \"commands: show, keys, scan, setup {json}, test, touch, touch cal, screen invert|mirror|bgr|rotate|bright|font|panel N, i2c sda scl, light r g b, restart, erase\"}");
    }
}

void console_start(void)
{
    xTaskCreate(console_task, "console", 8192, NULL, 3, NULL);   // scan brings the radio up from here
    ESP_LOGI(TAG, "serial console ready");
}
