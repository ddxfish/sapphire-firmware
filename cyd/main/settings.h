// settings.h - what the board is told once over USB, kept in flash (NVS).
// The WiFi password and the two keys sit in plain flash, as on any such
// board. The setup script never prints them back.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define SETTING_MAX 256                // any one text setting
#define CERT_MAX 4000                  // Sapphire's certificate, PEM (NVS holds 4000 at most)

typedef struct {
    char name[64];                     // the device name in Settings > Devices
    char wifi_ssid[SETTING_MAX];
    char wifi_password[SETTING_MAX];
    char key[SETTING_MAX];             // the key Sapphire sends
    char voice_key[SETTING_MAX];       // the key this board sends (the name is the protocol's)
    char sapphire[SETTING_MAX];        // https://192.168.0.10:8073
    char cert[CERT_MAX];               // Sapphire's certificate, or empty = not checked
    int brightness;                    // the backlight, 10 to 100
    int font;                          // the chat font in px: 14 to 24
    int home;                          // the page the board opens on (ui.h page_t)
    int volume;                        // her voice, 0..100 (speaker.c)
} settings_t;

#define BRIGHTNESS_DEFAULT 80
#define HOME_DEFAULT 0
#define VOLUME_DEFAULT 80

extern settings_t settings;

void settings_load(void);
bool settings_save(void);              // false when flash refused
void settings_erase(void);
bool settings_ready(void);             // enough to join WiFi and reach Sapphire
const char *board_mac(void);            // "aa:bb:cc:dd:ee:ff", the chip's own id
