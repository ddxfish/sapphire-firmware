// settings.h - what the board is told once over USB, kept in flash (NVS).
// The WiFi password and the two keys sit in plain flash, as on any such
// board. The setup script never prints them back.
#pragma once
#include "board.h"
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
    char sapphire[SETTING_MAX];        // https://192.0.2.10:8073
    char cert[CERT_MAX];               // Sapphire's certificate, or empty = not checked
    int volume;                        // the speaker, 0 to 100
    int mic_gain_x2;                   // the mic, in half decibels: 0..60 = 0..30 dB, digital (audio.c)
    int question_max_s;                // a question is cut off at this many seconds, however long the talk
    bool agc;                          // the AFE's automatic gain control; applied when the AFE starts
    bool invert;                       // the glass: inverted colors (this panel wants it, as Xiaozhi found)
    bool flip;                         // the glass: 180 degrees (worn with the lug up, the pendant hangs upside down to its board)
    bool mirror;                       // the glass: left and right swapped (a glass seen in a mirror, or scanned the other way)
    int brightness;                    // the glass: backlight while in use, 0 to 100
    int dim_percent;                   // the glass: backlight once left alone, 0 to 100
    int dim_after_s;                   // left alone this long, it dims; 0 = never
    int off_after_min;                 // left alone this long, the backlight goes out until a touch or she stirs; 0 = never
    int ring;                          // the ring: how bright its looks are painted, 5 to 100 percent of what each look asks
    bool ring_off;                     // the ring was turned off by hand or by her: dark through every state, until turned on
    int vol_step;                      // what one press of a volume button adds or takes, 1 to 50
    bool wake_off;                     // the wake word was turned off: it stays off across a restart
    bool mute;                         // the speaker was muted: the same
    int sleep_after_min;               // left alone this long, the board sleeps (power.c); 0 = never
    int aec_delay_ms;                  // the echo reference is read this long after it was played (audio.c)
    bool aec;                          // the echo canceller is fed the reference (off: silence, for comparison)
    int aec_filter;                    // the canceller's filter length, 1 to 8; applied when the AFE starts
    int afe_mode;                      // 0 = AFE low cost, 1 = high performance; applied when the AFE starts
#define FACE_LAG_MS_DEFAULT 700         // Krem heard ~800 ms on 2026-11-03; tuned by ear from the Devices page
#ifndef STREAM_FPS_DEFAULT
#define STREAM_FPS_DEFAULT 25          // Krem (2026-11-03): the face server's native rate; with one stream and region frames the pendant paints it at ~40% idle on both cores ("VERY smooth")
#endif
    int stream_fps;                    // the beat a stream of frames is painted on, 1-25 (regions since 0.2.0: the panel bus no longer caps it); /health screen.beat and screen.fps, PUT /screen/settings {"beat"}
    int face_lag_ms;                   // her voice held this long at the start of a reply while her face streams, so the two meet (the face takes the server's render time); 0-2000, Devices page "Face sync delay"
    int av_buffer;                     // packets gathered before her stream plays, 1-3 (avatar.c)
    bool check_boot;                   // read the newest backup back at boot; off by default (Krem, 2026-11-03: it ran over her face); landing and the button still check
    int speaker_rate;                  // her voice: 16000 or 24000 (the mic stays 16000); live, POST /mic?speaker_rate=
    int home_app;                      // the tile it opens on: the one it was left on (kept once a tile stays put 30 s)
    bool codec;                        // her voice comes as IMA ADPCM (a quarter of the air) instead of 16 bit PCM: /health plays.codec, POST /mic?codec=
    int touch_dy;                      // the glass: pixels added to every touch's y (the chip reads a few mm high; Elecrow adds 10), -40 to 40
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
// MIC_GAIN_X2_DEFAULT, MIC_GAIN_X2_MAX: the board's (boards/<name>.h)
#define SLEEP_AFTER_MIN_DEFAULT 0        // never (Krem 2026-11-08): a board that never sleeps never fails to wake; sleep is opt-in. Was an hour (2026-11-02); 10 slept the board the moment a backup landed
#define AEC_DELAY_MS_DEFAULT 0           // the sweep of 2026-11-02 on her voice: 0 ms cut the echo 34x, 16 ms 11x, 64 ms barely; the filter spans the 26-36 ms the sound takes to come back
#define AEC_FILTER_DEFAULT 4
#ifndef TOUCH_DY_DEFAULT
#define TOUCH_DY_DEFAULT 0
#endif

extern settings_t settings;

void settings_load(void);
bool settings_save(void);              // false when flash refused. Never from a task whose stack is in PSRAM: flash is written with the cache off
void settings_save_soon(void);         // from any task: saved two seconds after the last call, by settings_tick
void settings_flush(void);             // now, before a restart; internal stacks only
void settings_tick(void);              // every second, from a task with an internal stack (main.c's watch)
void settings_erase(void);
bool settings_ready(void);             // enough to join WiFi and reach Sapphire
const char *board_mac(void);            // "aa:bb:cc:dd:ee:ff", the chip's own id
