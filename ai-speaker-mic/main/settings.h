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
    char voice_key[SETTING_MAX];       // the key this board sends
    char sapphire[SETTING_MAX];        // https://192.168.0.10:8073
    char cert[CERT_MAX];               // Sapphire's certificate, or empty = not checked
    int volume;                        // the speaker, 0 to 100
    int mic_gain_x2;                   // the microphones, in half decibels: 0..75 = 0..37.5 dB (the ES7210's range)
    bool agc;                          // the AFE's automatic gain control; applied when the AFE starts
    bool invert;                       // the glass: inverted colors (this panel wants it, as Xiaozhi found)
    bool flip;                         // the glass: 180 degrees
    int brightness;                    // the glass: backlight while in use, 0 to 100
    int dim_percent;                   // the glass: backlight once left alone, 0 to 100
    int dim_after_s;                   // left alone this long, it dims; 0 = never
    int off_after_min;                 // left alone this long, the backlight goes out until a touch or she stirs; 0 = never
} settings_t;

#define BRIGHTNESS_DEFAULT 80
#define DIM_PERCENT_DEFAULT 15
#define DIM_AFTER_S_DEFAULT 120

#define VOLUME_DEFAULT 85
#define MIC_GAIN_X2_DEFAULT 60           // 30 dB, the value the first firmware hard-coded
#define MIC_GAIN_X2_MAX 75

extern settings_t settings;

void settings_load(void);
bool settings_save(void);              // false when flash refused
void settings_erase(void);
bool settings_ready(void);             // enough to join WiFi and reach Sapphire
const char *board_mac(void);            // "aa:bb:cc:dd:ee:ff", the chip's own id
