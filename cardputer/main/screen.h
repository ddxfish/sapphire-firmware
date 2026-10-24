// screen.h - the Cardputer's ST7789V2 240x135 glass as the walkie-talkie home
// page: a title and a one-word status. No LVGL, just an 8x8 font blitted over
// esp_lcd. Driven by light.c's cues (idle/listen/think/speak/nolink).
#pragma once

void screen_start(void);               // SPI + ST7789 + backlight, paint the home page
void screen_status(const char *cue);   // redraw the status word for a light cue (safe before start)
void screen_tabs(const char *tab, const char *const *items, int n, int sel, int tab_i, int tab_n);   // tabbed launcher

// Geometry tuning (console `screen ...`): find the right orientation/offset on
// glass without reflashing. test draws a border + corner markers; config
// re-applies swap/mirror/gap then redraws the test; home repaints the page.
void screen_test(void);
void screen_ruler(void);                 // gray fill + 8px edge bands (R/G/B/Y) to measure clip/offset
void screen_cal(void);                   // white-on-black: edges + centre line + alphabet (webcam calibration)
void screen_config(int flip_x, int flip_y, int gap_x, int gap_y);
void screen_home(void);
