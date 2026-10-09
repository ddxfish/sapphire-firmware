// ui.h - the glass: a window into her chat, with five places to touch.
//
//   ┌───────────────────────┐
//   │[ring]   name 21:50 [scr]│  corners: the ring, the screen's power
//   │                       │
//   │       (the scene)     │  the background is the scene of the chat this board
//   │         ( o )         │  talks in, pushed by Sapphire; the halo in the middle
//   │                       │  is the ring's color. A tap there: listen now, or
//   │   heard: what time..  │  stop her while she speaks.
//   │[wake]           [voice]│  corners: the wake word on/off, the speaker on/off
//   └───────────────────────┘
//
// Her `show` is a line across the top for a while; her `picture` takes the
// whole glass until a tap or its time, then the scene is back. Both images
// arrive as RGB565 big-endian and live in PSRAM; LVGL paints them.
// Every function here may be called from any task: each takes LVGL's lock,
// waits half a second for it at most, and says false when the glass did not answer.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UI_SCENE 0
#define UI_PICTURE 1

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
bool ui_image_drop(int kind);                     // begin without show, or clear
const char *ui_scene_name(void);                  // "" = none
bool ui_picture_on(void);
bool ui_screen_power(bool on);                    // backlight; a touch turns it back on
bool ui_screen_on(void);
void ui_test_card(void);                          // frame, TOP, color bars, a touch echo on the console
int ui_beat_age_ms(void);                         // since LVGL's task last ran its tick; -1 without a glass
int ui_doors_shut(void);                          // calls that gave up on the lock since boot
void ui_test_stuck(void);                         // console `fault glass`: holds the lock for good
bool ui_relight(void);                            // the brightness settings changed: apply them now
