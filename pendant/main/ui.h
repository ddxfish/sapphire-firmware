// ui.h - the glass: a row of apps, each with its status page below it.
//
//   ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐
//   │ BACKUPS  │ │ SETTINGS │ │  VOICE   │ │   FACE   │   swipe left and right; VOICE is home,
//   │ the card │ │ sliders, │ │ 21:50    │ │ her face │   the system pages sit on its left, the
//   │ back up  │ │ switches │ │  ( o )   │ │ streamed │   apps on its right
//   │ now      │ │ sleep    │ │ chat name│ │  here    │
//   ├──────────┤ ├──────────┤ ├──────────┤ ├──────────┤
//   │  card    │ │  board   │ │  status  │ │  stream  │   swipe up from any app: its status; down comes back
//   └──────────┘ └──────────┘ └──────────┘ └──────────┘
//
// VOICE is her chat as a scene with the halo (the light's color) in the middle, the
// clock, what she heard, the chat's name (tap: the picker), and two round buttons:
// the wake word and the speaker. FACE is where a stream lands: FlashHead's face, a
// game's table; a frame that comes while another app is open is answered hidden,
// unless a game asked for the taps, which opens FACE by itself. SETTINGS is the
// board's own knobs. Seven places still tile the glass by name for a game's taps
// (screen.zones) and for Sapphire's bindings of the halo and the two buttons.
//
// Her `show` is a line across the top for a while; her `picture` (a still) takes the
// whole glass, over every app, until a tap or its time. Both images arrive as
// RGB565 big-endian and live in PSRAM; LVGL paints them. A stream's JPEG frames are
// painted on a steady beat, UI_STREAM_FPS a second, the newest decoded frame each beat.
// Every function here may be called from any task: each takes LVGL's lock, waits
// half a second for it at most, and says false when the glass did not answer.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define UI_SCENE 0
#define UI_PICTURE 1
#define UI_STREAM_FPS (settings.stream_fps)   // the beat, a setting since 0.1.43 (PUT /screen/settings {"beat": N}); default STREAM_FPS_DEFAULT
enum { APP_BACKUP, APP_SETUP, APP_VOICE, APP_AVATAR, APPS };   // left to right; VOICE is home; AVATAR is hers alone (0.2.0: nothing on it but her)
typedef struct { const char *name; int x, y, w, h; } ui_zone_t;   // a place to touch, in glass pixels

void ui_start(void);
bool ui_ok(void);
bool ui_toast(const char *text, int seconds);     // her `show`; "" takes it down
bool ui_chat(const char *name, const char *brain, int trim_rgb);
const char *ui_chat_name(void);
const char *ui_brain_name(void);
int ui_trim(void);
void ui_heard(const char *text);                  // what she heard, for a while
void ui_status(const char *text);                 // the clock's place; "" = the clock
uint8_t *ui_image_begin(int kind, int w, int h);
bool ui_image_show(int kind, int w, int h, int seconds, const char *name);
bool ui_frame_ready(int w, int h, int seconds, bool taps, bool *replaced);
bool ui_image_drop(int kind);
bool ui_image_abort(int kind);
const char *ui_scene_name(void);
bool ui_picture_on(void);
bool ui_stream_hidden(void);                      // a stream was tapped away while she talks: its frames are not shown until her reply ends
bool ui_stream_hidden_for(bool taps);             // the frame door: hidden as above, or another app is open and no game asked
bool ui_stream_left(void);
bool ui_stream_taps(void);
int ui_zones(const ui_zone_t **zones);
bool ui_status_open(void);                        // the status page under the app in view (a button job: "status")
bool ui_chats_open(const char *joined, const char *current);
void ui_stream_counts(int *painted, int *replaced);
void ui_stream_gaps(int *mean_ms, int *max_ms, int *late);   // the last 32 paints: the gap between them, and how many came late (over 1.5x the window's mean)
bool ui_stream_live(void);
void ui_face_toggle(void);             // hide her face (frames answered hidden) or show it again
// Her stream (avatar.c): a decoded region lands on her face at (x, y) and goes to the panel at once when her tile is up.
bool ui_av_frame(const uint8_t *rgb565be, int x, int y, int w, int h, bool key, bool voice);   // voice: a frame of her speaking wakes a dimmed screen; her idle does not
bool ui_repaint(void);                 // everything drawn again (bench: after a gap change)
bool ui_av_shown(void);
const char *ui_av_refusal(void);       // why the last ui_av_frame returned false                // her tile is up on a lit screen: the sender keeps her idle full, else only a key now and then
void ui_av_show(void);                 // her voice starts: her tile opens
void ui_av_end(void);                  // a stream ended: her face stays a moment, then goes             // a frame painted in the last 3 s: her face is moving on the glass
bool ui_screen_power(bool on);
bool ui_screen_on(void);
int ui_app(void);                                 // the app in view (APP_*: 0 backups, 1 settings, 2 voice, 3 face)
bool ui_show_app(int app);                        // scroll to it
void ui_sleep(bool on);                           // power.c: dark and LVGL's lock held (true); back, repainted (false). Same task both ways
void ui_heal(void);                               // main.c watch task: the expander was reset and put back; the panel set up again, the glass repainted
int64_t ui_idle_ms(void);                         // since the last touch, frame, toast or her stirring; 0 without a glass
void ui_test_card(void);
void ui_touch_echo(int seconds);
cJSON *ui_touch_log(void);                        // the last touch-downs, newest first: [{"x", "y", "ms_ago"}], for /health (the bench)                  // every finger-down printed on the console as TOUCH x y, from LVGL's own reader
int ui_beat_age_ms(void);
int ui_doors_shut(void);
void ui_test_stuck(void);
bool ui_orient(void);
bool ui_relight(void);
