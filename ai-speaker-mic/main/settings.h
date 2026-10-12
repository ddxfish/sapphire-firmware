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
    int question_max_s;                // a question is cut off at this many seconds, however long the talk
    bool agc;                          // the AFE's automatic gain control; applied when the AFE starts
    bool invert;                       // the glass: inverted colors (this panel wants it, as Xiaozhi found)
    bool flip;                         // the glass: 180 degrees
    int brightness;                    // the glass: backlight while in use, 0 to 100
    int dim_percent;                   // the glass: backlight once left alone, 0 to 100
    int dim_after_s;                   // left alone this long, it dims; 0 = never
    int off_after_min;                 // left alone this long, the backlight goes out until a touch or she stirs; 0 = never
    int ring;                          // the ring: how bright its looks are painted, 5 to 100 percent of what each look asks
    bool ring_off;                     // the ring was turned off by hand or by her: dark through every state, until turned on
    int vol_step;                      // what one press of a volume button adds or takes, 1 to 50
    bool wake_off;                     // the wake word was turned off: it stays off across a restart
    bool mute;                         // the speaker was muted: the same
} settings_t;

#define BRIGHTNESS_DEFAULT 80
#define DIM_PERCENT_DEFAULT 15
#define DIM_AFTER_S_DEFAULT 120

#define QUESTION_MAX_S_DEFAULT 30
#define QUESTION_MAX_S_LEAST 5
#define QUESTION_MAX_S_MOST 60          // what the recording buffer holds (wake.c)

#define VOLUME_DEFAULT 85
#define VOL_STEP_DEFAULT 10
#define RING_LEAST 5                     // the slider's floor: a ring at 0 is what "off" is for
#define MIC_GAIN_X2_DEFAULT 60           // 30 dB, the value the first firmware hard-coded
#define MIC_GAIN_X2_MAX 75

extern settings_t settings;

void settings_load(void);
bool settings_save(void);              // false when flash refused. Never from a task whose stack is in PSRAM: flash is written with the cache off
void settings_save_soon(void);         // from any task: saved two seconds after the last call, by settings_tick
void settings_flush(void);             // now, before a restart; internal stacks only
void settings_tick(void);              // every second, from a task with an internal stack (main.c's watch)
void settings_erase(void);
bool settings_ready(void);             // enough to join WiFi and reach Sapphire
const char *board_mac(void);            // "aa:bb:cc:dd:ee:ff", the chip's own id
