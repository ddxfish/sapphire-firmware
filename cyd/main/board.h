// board.h - the board under the reader: one of the "Cheap Yellow Displays",
// chosen when the program is built (`build.sh cyd28` or `cyd35`: BOARD_PINS
// names the board's own header in boards/, and boards/<name>.c holds its
// glass and its touch chip). Everything above this file is the same program,
// built twice (2026-10-16: the two trees had drifted apart within a week).
//
// The panels are portrait glass turned in hardware: `rotation` is kept in
// flash and sets the panel's axes, the size the screen is built at (board_w,
// board_h) and how a touch is mapped. A touch chip reports in the panel's
// native frame (PANEL_W x PANEL_H; a resistive glass gets there through its
// calibration), and board_touch turns that into the screen as it sits now.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "nvs.h"

#include BOARD_PINS                   // boards/cyd28.h or boards/cyd35.h: pins, the panel's size, its quirks

// The draw buffer LVGL paints into, in pixels: 320 x 24 x 2 bytes, twice =
// 30 KB of DMA-capable RAM on either board (a whole screen would be 150 to
// 300 KB: not here). server.c paints pictures in strips of PICTURE_PIXELS.
#define BOARD_DRAW_PIXELS (320 * 24)
#define BOARD_PICTURE_PIXELS (320 * 12)

typedef enum { PANEL_AUTO = 0, PANEL_ILI9341, PANEL_ST7789, PANEL_ST7796 } panel_t;

typedef struct { bool swap, mirror_x, mirror_y; } turn_t;   // how the panel is told to sit (MADCTL); the touch follows

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    panel_t panel_set;                 // the console's word (`screen panel`), kept; AUTO = the board's own guess
    panel_t panel_is;                  // what runs now
    uint8_t id[4];                     // what the chip answered, for `show` (on a board that asks)
    bool touch;                        // the touch chip answered at boot
    bool invert;                       // the panel wants inverted colors; the test card says
    bool bgr;                          // the panel takes blue first; restarts
    bool bgr_kept;                     // flash holds a word on it (`screen bgr`); else the board file decides by the chip it finds
    int rotation;                      // 0, 90, 180, 270: how the glass sits. 0 = landscape; the test card's TOP says the rest
    bool mirror;                       // the X axis the other way, on top of that: for a glass that scans differently
} board_t;

#define CAL_INSET 24                   // the calibration crosses sit this far in from the corners

bool board_init(void);                 // flash words, backlight, LED, light sensor, button, then the glass and the touch
board_t *board(void);
int board_w(void);                     // the screen as it sits now
int board_h(void);
turn_t board_turn(void);               // what the panel was told; ui.c tells LVGL's port the same
void board_backlight(int percent);     // 0 = dark
void board_led(uint8_t r, uint8_t g, uint8_t b);
int board_light_raw(void);             // the light sensor, 0..4095; -1 when it cannot be read
bool board_button(void);               // true while BOOT is held
void board_set_invert(bool on);        // kept; takes effect at once
void board_set_rotation(int degrees);  // kept; the next start (the screen is built at a size)
void board_set_mirror(bool on);        // kept; the panel is turned at once (ui.c redraws)
void board_set_panel(panel_t which);   // kept; the next start
void board_set_bgr(bool on);           // kept; the next start
bool board_touch(int *x, int *y);      // a finger now, in screen coordinates (board_w x board_h)
bool board_touch_calibrate(const int raw[3][2], const int at[3][2]);   // raw readings at three crosses, and where they were drawn; false = refused
void board_to_native(int x, int y, int *nx, int *ny);     // the screen as it sits -> the panel's own frame
void board_from_native(int nx, int ny, int *x, int *y);

// --- what boards/<name>.c provides ----------------------------------------------------------------
bool board_glass_up(void);             // the SPI bus, the panel io and the panel (b->io, b->panel), reset and init done
bool board_touch_up(void);             // the touch chip's pins or bus; true when it answers
bool board_touch_raw(int *x, int *y, int *z);   // the chip's own numbers now: true while touched (z = pressure; 1 on a capacitive glass)
bool board_touch_native(int *x, int *y);   // a finger now, in the panel's native frame
bool board_touch_keep(const int raw[3][2], const int native[3][2]);   // a calibration: raw readings at three native points; false = refused, or needs none
bool board_touch_needs_cal(void);      // a resistive glass: the test card offers to calibrate, the Status page says if it was
bool board_touch_calibrated(void);     // one is kept (a capacitive glass: always)
const char *board_touch_words(void);   // "calibrated", "NOT calibrated: touch cal", "capacitive"
const char *board_panel_name(void);
void board_load_own(nvs_handle_t h);   // the board's own flash words (a calibration), inside board.c's load and save
void board_save_own(nvs_handle_t h);
