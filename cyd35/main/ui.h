// ui.h - the screen: one chat, portrait, a keyboard that slides up.
//
//   ┌──────────────────────┐
//   │ ● pocket      12:34  │  the status bar: link dot, name, her state / the clock
//   │                      │
//   │   [her bubble]       │  the chat window: the last N bubbles, newest at the
//   │          [yours]     │  bottom. The whole history stays with Sapphire.
//   │                      │
//   │ [type here.......][>]│  the input row
//   │ q w e r t y u i o p  │  lv_keyboard, shown while the input has focus
//   └──────────────────────┘
//
// Every function here may be called from any task: each takes LVGL's lock.
// Her reply arrives in pieces (link.c): ui_reply_begin, then ui_reply_set with
// everything so far, then ui_reply_done.
#pragma once
#include <stdbool.h>
#include <stddef.h>

void ui_start(void);
void ui_status(const char *text);          // the status bar's right side; "" = the clock
void ui_link(bool connected);              // the dot
void ui_reply_begin(void);                 // a new bubble of hers starts
void ui_reply_set(const char *text);       // everything of her reply so far
void ui_reply_done(void);
void ui_failed(const char *why);           // the last thing you typed did not reach her
void ui_mine(const char *text);            // a bubble of yours that was not typed: what a button sent
void ui_toast(const char *text, int seconds);   // a line across the top, for a while
void ui_test_card(void);                   // frame, TOP, color bars, a touch echo on the serial line
void ui_wake(void);                        // the backlight back on
// A picture takes the whole glass, painted straight onto the panel by whoever
// holds it (server.c). LVGL is held still meanwhile; what she says in that
// time is kept and drawn when the picture goes.
bool ui_picture_begin(void);               // false: the screen is busy
void ui_picture_end(void);
