// settings.c - NVS namespace "sat", one key per field.
#include "settings.h"

#include <stdio.h>
#include "esp_mac.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
settings_t settings;

static const struct { const char *key; char *at; size_t size; } FIELDS[] = {
    {"name", settings.name, sizeof settings.name},
    {"wifi_ssid", settings.wifi_ssid, sizeof settings.wifi_ssid},
    {"wifi_password", settings.wifi_password, sizeof settings.wifi_password},
    {"key", settings.key, sizeof settings.key},
    {"voice_key", settings.voice_key, sizeof settings.voice_key},
    {"sapphire", settings.sapphire, sizeof settings.sapphire},
    {"cert", settings.cert, sizeof settings.cert},
};
#define N_FIELDS (sizeof FIELDS / sizeof FIELDS[0])

void settings_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    memset(&settings, 0, sizeof settings);
    strcpy(settings.name, "satellite");
    settings.volume = VOLUME_DEFAULT;
    settings.mic_gain_x2 = MIC_GAIN_X2_DEFAULT;
    settings.agc = false;
    settings.question_max_s = QUESTION_MAX_S_DEFAULT;
    settings.invert = true;
    settings.flip = false;
    settings.brightness = BRIGHTNESS_DEFAULT;
    settings.dim_percent = DIM_PERCENT_DEFAULT;
    settings.dim_after_s = DIM_AFTER_S_DEFAULT;
    settings.off_after_min = 0;
    nvs_handle_t h;
    if (nvs_open("sat", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no settings yet: waiting to be set up over USB");
        return;
    }
    for (size_t i = 0; i < N_FIELDS; i++) {
        size_t size = FIELDS[i].size;
        if (nvs_get_str(h, FIELDS[i].key, FIELDS[i].at, &size) != ESP_OK)
            FIELDS[i].at[0] = 0;
    }
    uint8_t volume;
    if (nvs_get_u8(h, "volume", &volume) == ESP_OK && volume <= 100) settings.volume = volume;
    uint8_t v8;
    if (nvs_get_u8(h, "mic_gain_x2", &v8) == ESP_OK && v8 <= MIC_GAIN_X2_MAX) settings.mic_gain_x2 = v8;
    if (nvs_get_u8(h, "agc", &v8) == ESP_OK) settings.agc = v8 != 0;
    if (nvs_get_u8(h, "q_max_s", &v8) == ESP_OK && v8 >= QUESTION_MAX_S_LEAST && v8 <= QUESTION_MAX_S_MOST) settings.question_max_s = v8;
    if (nvs_get_u8(h, "invert", &v8) == ESP_OK) settings.invert = v8 != 0;
    if (nvs_get_u8(h, "flip", &v8) == ESP_OK) settings.flip = v8 != 0;
    if (nvs_get_u8(h, "bright", &v8) == ESP_OK && v8 <= 100) settings.brightness = v8;
    uint16_t v16;
    if (nvs_get_u8(h, "dim", &v8) == ESP_OK && v8 <= 100) settings.dim_percent = v8;
    if (nvs_get_u16(h, "dim_after", &v16) == ESP_OK) settings.dim_after_s = v16;
    if (nvs_get_u16(h, "off_after", &v16) == ESP_OK) settings.off_after_min = v16;
    nvs_close(h);
    if (!settings.name[0]) strcpy(settings.name, "satellite");
}

bool settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("sat", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = true;
    for (size_t i = 0; i < N_FIELDS; i++)
        ok = ok && nvs_set_str(h, FIELDS[i].key, FIELDS[i].at) == ESP_OK;
    ok = ok && nvs_set_u8(h, "volume", settings.volume) == ESP_OK;
    ok = ok && nvs_set_u8(h, "mic_gain_x2", settings.mic_gain_x2) == ESP_OK;
    ok = ok && nvs_set_u8(h, "agc", settings.agc ? 1 : 0) == ESP_OK;
    ok = ok && nvs_set_u8(h, "q_max_s", settings.question_max_s) == ESP_OK;
    ok = ok && nvs_set_u8(h, "invert", settings.invert ? 1 : 0) == ESP_OK;
    ok = ok && nvs_set_u8(h, "flip", settings.flip ? 1 : 0) == ESP_OK;
    ok = ok && nvs_set_u8(h, "bright", settings.brightness) == ESP_OK;
    ok = ok && nvs_set_u8(h, "dim", settings.dim_percent) == ESP_OK;
    ok = ok && nvs_set_u16(h, "dim_after", settings.dim_after_s) == ESP_OK;
    ok = ok && nvs_set_u16(h, "off_after", settings.off_after_min) == ESP_OK;
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void settings_erase(void)
{
    nvs_handle_t h;
    if (nvs_open("sat", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    memset(&settings, 0, sizeof settings);
    strcpy(settings.name, "satellite");
    settings.volume = VOLUME_DEFAULT;
    settings.mic_gain_x2 = MIC_GAIN_X2_DEFAULT;
    settings.agc = false;
    settings.question_max_s = QUESTION_MAX_S_DEFAULT;
    settings.invert = true;
    settings.flip = false;
    settings.brightness = BRIGHTNESS_DEFAULT;
    settings.dim_percent = DIM_PERCENT_DEFAULT;
    settings.dim_after_s = DIM_AFTER_S_DEFAULT;
    settings.off_after_min = 0;
}

bool settings_ready(void)
{
    return settings.wifi_ssid[0] && settings.key[0] && settings.voice_key[0] && settings.sapphire[0];
}

// The chip's factory MAC as "aa:bb:cc:dd:ee:ff": what the flasher read over
// USB, so Sapphire knows this board again whatever it is called.
const char *board_mac(void)
{
    static char text[18];
    if (!text[0]) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return text;
}
