// settings.c - NVS namespace "pocket", one key per field.
#include "settings.h"

#include "board.h"

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

static void defaults(void)
{
    memset(&settings, 0, sizeof settings);
    strcpy(settings.name, "pocket");
    settings.brightness = BRIGHTNESS_DEFAULT;
    settings.font = BOARD_FONT_DEFAULT;     // the 2.8" reads at 16, the 3.5" at 18
    settings.home = HOME_DEFAULT;
    settings.volume = VOLUME_DEFAULT;
}

void settings_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    defaults();
    nvs_handle_t h;
    if (nvs_open("pocket", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no settings yet: waiting to be set up over USB");
        return;
    }
    for (size_t i = 0; i < N_FIELDS; i++) {
        size_t size = FIELDS[i].size;
        if (nvs_get_str(h, FIELDS[i].key, FIELDS[i].at, &size) != ESP_OK)
            FIELDS[i].at[0] = 0;
    }
    uint8_t v8;
    if (nvs_get_u8(h, "brightness", &v8) == ESP_OK && v8 >= 10 && v8 <= 100) settings.brightness = v8;
    if (nvs_get_u8(h, "font", &v8) == ESP_OK && (v8 == 14 || v8 == 16 || v8 == 18 || v8 == 20 || v8 == 22 || v8 == 24)) settings.font = v8;
    if (nvs_get_u8(h, "home", &v8) == ESP_OK && v8 < 8) settings.home = v8;
    if (nvs_get_u8(h, "volume", &v8) == ESP_OK && v8 <= 100) settings.volume = v8;
    nvs_close(h);
    if (!settings.name[0]) strcpy(settings.name, "pocket");
}

bool settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("pocket", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = true;
    for (size_t i = 0; i < N_FIELDS; i++)
        ok = ok && nvs_set_str(h, FIELDS[i].key, FIELDS[i].at) == ESP_OK;
    ok = ok && nvs_set_u8(h, "brightness", settings.brightness) == ESP_OK;
    ok = ok && nvs_set_u8(h, "font", settings.font) == ESP_OK;
    ok = ok && nvs_set_u8(h, "home", settings.home) == ESP_OK;
    ok = ok && nvs_set_u8(h, "volume", settings.volume) == ESP_OK;
    ok = ok && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void settings_erase(void)
{
    nvs_handle_t h;
    if (nvs_open("pocket", NVS_READWRITE, &h) == ESP_OK) {
        // only what is set up here: the panel's knobs, the touch calibration and the LED looks share the namespace and stay
        for (size_t i = 0; i < N_FIELDS; i++) nvs_erase_key(h, FIELDS[i].key);
        for (const char **k = (const char *[]){"brightness", "font", "home", "volume", NULL}; *k; k++) nvs_erase_key(h, *k);
        nvs_commit(h);
        nvs_close(h);
    }
    defaults();
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
