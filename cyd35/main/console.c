// console.c - see console.h.
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_private/log_lock.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "light.h"
#include "net.h"
#include "settings.h"
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
    cJSON_AddBoolToObject(j, "touch", board()->touch != NULL);
    cJSON_AddBoolToObject(j, "invert", board()->invert);
    cJSON_AddBoolToObject(j, "flip", board()->flip);
    cJSON_AddNumberToObject(j, "brightness", settings.brightness);
    cJSON_AddNumberToObject(j, "font", settings.font);
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

static void cmd_screen(const char *rest)
{
    char what[16] = {0};
    int value = -1;
    if (sscanf(rest, "%15s %d", what, &value) < 2) { answer_text("{\"error\": \"screen invert|flip 0|1, bright 10..100, font 14|16|18|20\"}"); return; }
    if (!strcmp(what, "invert")) board_set_invert(value != 0);
    else if (!strcmp(what, "flip")) board_set_flip(value != 0);
    else if (!strcmp(what, "bright")) {
        settings.brightness = value < 10 ? 10 : value > 100 ? 100 : value;
        settings_save();
        board_backlight(settings.brightness);
    }
    else if (!strcmp(what, "font")) {
        if (value != 14 && value != 16 && value != 18 && value != 20) { answer_text("{\"error\": \"font 14, 16, 18 or 20\"}"); return; }
        settings.font = value;
        settings_save();
        answer_text("{\"ok\": true, \"restarting\": true}");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    else { answer_text("{\"error\": \"screen invert|flip|bright\"}"); return; }
    answer_text("{\"ok\": true}");
}

// i2c <sda> <scl> [rst]: who answers on those two pins. For finding the touch
// chip on a board whose pinout is folklore. With rst, that pin is pulled low
// then high first, so a chip held in reset gets to answer.
static void cmd_i2c_scan(const char *rest)
{
    int sda = -1, scl = -1, rst = -1;
    if (sscanf(rest, "%d %d %d", &sda, &scl, &rst) < 2 || sda < 0 || scl < 0) { answer_text("{\"error\": \"i2c <sda> <scl> [rst]\"}"); return; }
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

// touch: the chip read directly for 10 seconds, no LVGL in between
static void cmd_touch(void)
{
    esp_lcd_touch_handle_t tp = board()->touch;
    if (!tp) { answer_text("{\"error\": \"no touch chip\"}"); return; }
    answer_text("{\"ok\": true, \"seconds\": 10}");
    int seen = 0;
    for (int i = 0; i < 200; i++) {
        esp_lcd_touch_read_data(tp);
        uint16_t x[5], y[5], s[5];
        uint8_t n = 0;
        if (esp_lcd_touch_get_coordinates(tp, x, y, s, &n, 5) && n) {
            printf("RAW %u %u %u\n", x[0], y[0], n);
            fflush(stdout);
            seen++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    char t[64];
    snprintf(t, sizeof t, "{\"ok\": true, \"touches\": %d}", seen);
    answer_text(t);
}

// gt: the GT911's own registers, raw, for 10 seconds: 0x814E status (bit 7 =
// points waiting, low bits = how many), 0x8040 command (0 = reading). Then a
// 0 written to the command register, in case it sat in another mode.
static void cmd_gt(void)
{
    esp_lcd_panel_io_handle_t io = board()->touch_io;
    if (!io) { answer_text("{\"error\": \"no touch chip\"}"); return; }
    answer_text("{\"ok\": true, \"seconds\": 10}");
    for (int i = 0; i < 40; i++) {
        uint8_t st = 0xEE, cmd = 0xEE, pt[4] = {0};
        esp_lcd_panel_io_rx_param(io, 0x814E, &st, 1);
        esp_lcd_panel_io_rx_param(io, 0x8040, &cmd, 1);
        if (st & 0x80) {
            esp_lcd_panel_io_rx_param(io, 0x8150, pt, 4);
            uint8_t zero = 0;
            esp_lcd_panel_io_tx_param(io, 0x814E, &zero, 1);
        }
        printf("GT status %02x cmd %02x x %u y %u\n", st, cmd, pt[0] | pt[1] << 8, pt[2] | pt[3] << 8);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    uint8_t zero = 0;
    esp_lcd_panel_io_tx_param(io, 0x8040, &zero, 1);
    answer_text("{\"ok\": true, \"done\": true}");
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
        else if (!strcmp(line, "gt")) cmd_gt();
        else if (!strncmp(line, "light ", 6)) {
            int r = 0, g = 0, b = 0;
            sscanf(line + 6, "%d %d %d", &r, &g, &b);
            answer_text("{\"ok\": true}");
            light_show(r & 255, g & 255, b & 255, 6);
        }
        else if (!strcmp(line, "restart")) { answer_text("{\"ok\": true}"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
        else if (!strcmp(line, "erase")) { settings_erase(); answer_text("{\"ok\": true, \"erased\": true}"); }
        else answer_text("{\"error\": \"commands: show, keys, scan, setup {json}, test, screen invert|flip|bright N, light r g b, restart, erase\"}");
    }
}

void console_start(void)
{
    xTaskCreate(console_task, "console", 8192, NULL, 3, NULL);   // scan brings the radio up from here
    ESP_LOGI(TAG, "serial console ready");
}
