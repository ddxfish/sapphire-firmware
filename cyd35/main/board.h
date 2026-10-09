// board.h - the 3.5" "Cheap Yellow Display", capacitive kind (ESP32-3248S035C):
// ESP32-D0WD-V3 in a WROOM module (no PSRAM, 4 MB flash), 3.5" ST7796 320x480
// on HSPI, a GT911 touch chip on I2C, one RGB LED, a light sensor, an
// amplifier on the DAC pin, a TF slot on VSPI, the BOOT button. Another board
// gets its own board.c with the same functions.
//
// Pins read from witnessmenow's PINS.md and f1atb's variant table. The board
// was bought as a 2.8"; what it is was read off the silicon (2026-10-06): the
// touch chip answers GT911 at 0x5D (the console's `i2c 33 32` scan) and its
// config says the panel is 320 x 480. The capacitive boards moved the
// backlight from 21 to 27 and put the touch chip on the old XPT2046 pins.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#define BOARD_NAME "cyd-3.5-cap"

#define BOARD_LCD_SCK 14
#define BOARD_LCD_MOSI 13
#define BOARD_LCD_MISO 12
#define BOARD_LCD_CS 15
#define BOARD_LCD_DC 2
#define BOARD_LCD_BL 27                // PWM, active high
#define BOARD_LCD_HZ (40 * 1000 * 1000)
#define BOARD_W 320
#define BOARD_H 480

#define BOARD_TP_SDA 33
#define BOARD_TP_SCL 32
#define BOARD_TP_RST 25
#define BOARD_TP_INT 21                // soldered to GND on this board (routing mistake): the chip is polled

#define BOARD_LED_R 4                  // all three active low
#define BOARD_LED_G 16
#define BOARD_LED_B 17

#define BOARD_LDR 34                   // ADC1 channel 6, input only
#define BOARD_BOOT_BUTTON 0            // low when pressed
#define BOARD_AMP 26                   // DAC channel 1 into the SC8002B amplifier (speaker.c)

#define BOARD_SD_SPI                   // the card is on a plain SPI bus (storage.c), VSPI
#define BOARD_SD_HOST SPI3_HOST
#define BOARD_SD_CS 5
#define BOARD_SD_SCK 18
#define BOARD_SD_MISO 19
#define BOARD_SD_MOSI 23
#define BOARD_STORAGE_CAPS MALLOC_CAP_DEFAULT   // no PSRAM: the write buffer is internal, and small
#define BOARD_STORAGE_CHUNK 4096

#define SPEAKER_RATE 22050             // what /health says in `plays`; Sapphire fits the sound to it.
                                       // 22.05 kHz keeps the DAC on its default clock (19.6 kHz is its floor)

// The draw buffer LVGL paints into, in lines. 24 lines x 320 x 2 bytes, twice
// = 31 KB of DMA-capable RAM. The whole screen would be 300 KB: not here.
#define BOARD_DRAW_LINES 24

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    esp_lcd_touch_handle_t touch;
    esp_lcd_panel_io_handle_t touch_io;   // the chip's I2C door, for the console's raw reads
    bool invert;                       // the panel wants inverted colors; the test card says
    bool flip;                         // 180 degrees: the USB plug at the top instead of the bottom
} board_t;

bool board_init(void);                 // SPI, panel, I2C, touch, backlight, LED pins, ADC, button
board_t *board(void);
void board_backlight(int percent);     // 0 = dark
void board_led(uint8_t r, uint8_t g, uint8_t b);
int board_light_raw(void);             // the light sensor, 0..4095; -1 when it cannot be read
bool board_button(void);               // true while BOOT is held
void board_set_invert(bool on);        // kept in flash; takes effect at once
void board_set_flip(bool on);
