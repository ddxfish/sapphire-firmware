// board.h - the 2.8" "Cheap Yellow Display", resistive kind (ESP32-2432S028R):
// ESP32-D0WD in a WROOM module (no PSRAM, 4 MB flash), a 2.8" 240x320 panel on
// HSPI (an ILI9341, or an ST7789 on the two-USB revision: the chip is asked
// at boot), an XPT2046 touch chip on wires of its own, one RGB LED, a light
// sensor, an amplifier on the DAC pin, a TF slot on VSPI, the BOOT button.
// Used in landscape: 320 wide, 240 tall.
//
// Pins from witnessmenow's PINS.md and the mischianti schematic (2026-10-15).
// Two SPI hosts, three buses: the panel holds HSPI, the card holds VSPI, so
// the touch chip is driven by hand on its pins (board_touch_raw). Its MISO
// (39) and IRQ (36) are input-only pins; ESP32 errata 3.11 says both glitch
// low for ~80 ns when ADC1 samples (the light sensor does), so IRQ is only a
// gate here and the chip's pressure reading says whether anyone is touching.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#define BOARD_NAME "cyd-2.8-res"

#define BOARD_LCD_SCK 14
#define BOARD_LCD_MOSI 13
#define BOARD_LCD_MISO 12
#define BOARD_LCD_CS 15
#define BOARD_LCD_DC 2
#define BOARD_LCD_BL 21                // PWM, active high
#define BOARD_LCD_HZ (40 * 1000 * 1000)
#define BOARD_LCD_READ_HZ (5 * 1000 * 1000)   // a panel answers its ID only at a slow clock
#define BOARD_PANEL_W 240              // the panel as mounted: portrait
#define BOARD_PANEL_H 320
#define BOARD_W 320                    // the screen as used: landscape
#define BOARD_H 240

#define BOARD_TP_CLK 25
#define BOARD_TP_MOSI 32
#define BOARD_TP_MISO 39               // input only
#define BOARD_TP_CS 33
#define BOARD_TP_IRQ 36                // input only; low while touched, not always (board_touch_raw reads pressure instead)

#define BOARD_LED_R 4                  // all three active low
#define BOARD_LED_G 16
#define BOARD_LED_B 17

#define BOARD_LDR 34                   // ADC1 channel 6, input only
#define BOARD_BOOT_BUTTON 0            // low when pressed
#define BOARD_AMP 26                   // DAC channel 1 into the SC8002B amplifier (speaker.c); 25 is the touch clock

#define BOARD_SD_SPI                   // the card is on a plain SPI bus (storage.c), VSPI
#define BOARD_SD_HOST SPI3_HOST
#define BOARD_SD_CS 5
#define BOARD_SD_SCK 18
#define BOARD_SD_MISO 19
#define BOARD_SD_MOSI 23
#define BOARD_STORAGE_CAPS MALLOC_CAP_DEFAULT   // no PSRAM: the write buffer is internal, and small
#define BOARD_STORAGE_CHUNK 4096

#define SPEAKER_RATE 22050             // what /health says in `plays`; Sapphire fits the sound to it

// The draw buffer LVGL paints into, in lines. 24 lines x 320 x 2 bytes, twice
// = 30 KB of DMA-capable RAM. The whole screen would be 150 KB: not here.
#define BOARD_DRAW_LINES 24

typedef enum { PANEL_AUTO = 0, PANEL_ILI9341, PANEL_ST7789 } panel_t;

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    panel_t panel_set;                 // the console's word (`screen panel`), kept; AUTO = ask the chip
    panel_t panel_is;                  // what runs now
    uint8_t id[4];                     // what the chip answered, for `show`
    bool touch;                        // the touch chip answered at boot
    bool invert;                       // the panel wants inverted colors; the test card says
    bool bgr;                          // the panel takes blue first (the cyd35 did; this one takes red first); restarts
    bool flip;                         // 180 degrees: the USB plug on the other side
    bool mirror;                       // the X axis the other way: with flip, every way the glass can sit
    bool cal_set;                      // a calibration is kept
    int cal[4];                        // raw at the left and right crosses, at the top and bottom crosses
    bool cal_swap;                     // the chip's X channel runs down the screen, not across it
    bool cal_flip, cal_mirror;         // how the glass sat when it was calibrated
} board_t;

#define CAL_INSET 24                   // the calibration crosses sit this far in from the corners

bool board_init(void);                 // SPI, panel, touch pins, backlight, LED pins, ADC, button
board_t *board(void);
void board_backlight(int percent);     // 0 = dark
void board_led(uint8_t r, uint8_t g, uint8_t b);
int board_light_raw(void);             // the light sensor, 0..4095; -1 when it cannot be read
bool board_button(void);               // true while BOOT is held
void board_set_invert(bool on);        // kept in flash; takes effect at once
void board_set_flip(bool on);          // kept; the panel is turned at once, LVGL redraws (ui.c is told)
void board_set_mirror(bool on);
void board_set_panel(panel_t which);   // kept; takes effect at the next start
void board_set_bgr(bool on);           // kept; takes effect at the next start
const char *board_panel_name(void);
bool board_touch_raw(int *x, int *y, int *z);   // the chip now: true while pressed; raw 12-bit X, Y and pressure
void board_touch_probe(int *irq, int *z1, int *z2, int *x, int *y);   // every reading, gate or no gate: for bring-up
bool board_touch(int *x, int *y);      // true while pressed; in screen coordinates, BOARD_W x BOARD_H
bool board_touch_calibrate(const int raw[3][2]);   // false: the taps were on one spot, nothing kept   // raw at the top-left, top-right and bottom-right crosses; kept
