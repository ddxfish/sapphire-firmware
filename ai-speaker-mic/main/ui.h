// ui.h - the glass: a window into her chat, with seven places to touch.
//
//   ┌──────┬──────────────┬──────┐
//   │[ring]│  21:50 (top) │ [scr]│  corners: the ring, the screen's power
//   │      ├──────────────┤      │
//   │      │ (the scene)  │      │  the background is the scene of the chat this board
//   │      │    ( o )     │      │  talks in, pushed by Sapphire; the halo in the middle
//   │      │   (middle)   │      │  is the ring's color. A tap there: listen now, or
//   │      ├──────────────┤      │  stop her while she speaks. The top: the status page.
//   │[wake]│ name (bottom)│[voice]│  corners: the wake word on/off, the speaker on/off
//   └──────┴──────────────┴──────┘
//
// The seven places tile the glass (0.9.0): every tap lands in one, and each is a
// button by name (main.c BUTTONS), so Sapphire may set what any of them does.
// A stream whose sender asks for the taps (?taps=1) gets each tap's place by name
// (link_tap) and no tap dismisses it; held anywhere, it is left (the next frame is
// answered hidden). The table is said in /health as screen.zones.
//
// Her `show` is a line across the top for a while; her `picture` takes the
// whole glass until a tap or its time, then the scene is back. Both images
// arrive as RGB565 big-endian and live in PSRAM; LVGL paints them.
// A stream (0.7.8): JPEG frames sent one after another are painted on a steady
// beat, UI_STREAM_FPS a second, the newest decoded frame each beat.
// Every function here may be called from any task: each takes LVGL's lock,
// waits half a second for it at most, and says false when the glass did not answer.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_SCENE 0
#define UI_PICTURE 1
#define UI_STREAM_FPS 3                // the beat a stream of frames is painted on (said in /health as screen.fps)
typedef struct { const char *name; int x, y, w, h; } ui_zone_t;   // a place to touch, in glass pixels

void ui_start(void);
bool ui_ok(void);
bool ui_toast(const char *text, int seconds);     // her `show`; "" takes it down
// The chat this board talks in: its name bottom middle ("" hides it), the model that answers in it
// (the status page), and its trim color as 0xRRGGBB or -1 (the middle wears it while she rests).
bool ui_chat(const char *name, const char *brain, int trim_rgb);
const char *ui_chat_name(void);
const char *ui_brain_name(void);
int ui_trim(void);
void ui_heard(const char *text);                  // what she heard, near the bottom, for a while
void ui_status(const char *text);                 // the top line's right side; "" = the clock
// An image: begin gives a PSRAM buffer of w*h pixels to fill with RGB565 as it
// arrives (big-endian, as the wire has it); show swaps it to LVGL's order and
// paints it. A scene stays until replaced or cleared; a picture goes after
// `seconds` or a tap. NULL when the glass is busy or out of memory.
uint8_t *ui_image_begin(int kind, int w, int h);
bool ui_image_show(int kind, int w, int h, int seconds, const char *name);
// A frame of a stream (a JPEG decoded big-endian): queued for the beat, the newest
// wins. The first frame of a stream is painted at once. `replaced` says whether a
// frame already waiting was dropped for this one. The stream ends `seconds` after its last frame.
// `taps`: the sender wants the taps (each one's place by name, link_tap) and no tap ends the stream.
bool ui_frame_ready(int w, int h, int seconds, bool taps, bool *replaced);
bool ui_image_drop(int kind);                     // begin without show, or clear
bool ui_image_abort(int kind);                    // from the one filling it: its upload broke off (a picture keeps the one before)
const char *ui_scene_name(void);                  // "" = none
bool ui_picture_on(void);
bool ui_stream_hidden(void);                      // a stream was tapped away while she talks: its frames are not shown until her reply ends
bool ui_stream_left(void);                        // a tapping stream was held and left: true once, for the frame that is answered hidden
bool ui_stream_taps(void);                        // a stream that owns the taps is up
int ui_zones(const ui_zone_t **zones);            // the seven places, for /health screen.zones
bool ui_status_open(void);                        // the status page (a button job: "status")
bool ui_chats_open(const char *joined, const char *current);   // the chat picker (0.9.1): her list one name a line, drops up from the chat's name; the pick goes by link_pick_chat
void ui_stream_counts(int *painted, int *replaced);   // since boot
bool ui_screen_power(bool on);                    // backlight; a touch turns it back on
bool ui_screen_on(void);
void ui_test_card(void);                          // frame, TOP, color bars, a touch echo on the console
int ui_beat_age_ms(void);                         // since LVGL's task last ran its tick; -1 without a glass
int ui_doors_shut(void);                          // calls that gave up on the lock since boot
void ui_test_stuck(void);                         // console `fault glass`: holds the lock for good
bool ui_orient(void);                             // settings.invert and settings.flip to the panel, under LVGL's lock
bool ui_relight(void);                            // the brightness settings changed: apply them now
