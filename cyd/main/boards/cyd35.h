// NAME: CYD 3.5" reader (ESP32-3248S035C, capacitive touch)
// boards/cyd35.h - the 3.5" "Cheap Yellow Display", capacitive kind
// (ESP32-3248S035C): ESP32-D0WD-V3 in a WROOM module (no PSRAM, 4 MB flash),
// a 3.5" ST7796 320x480 on HSPI, a GT911 touch chip on I2C, one RGB LED, a
// light sensor, an amplifier on the DAC pin, a TF slot on VSPI, the BOOT
// button.
//
// Pins read from witnessmenow's PINS.md and f1atb's variant table. The board
// was bought as a 2.8"; what it is was read off the silicon (2026-10-06): the
// touch chip answers GT911 at 0x5D (the console's `i2c 33 32` scan) and its
// config says the panel is 320 x 480. The capacitive boards moved the
// backlight from 21 to 27 and put the touch chip on the old XPT2046 pins.
// GPIO 21 is the touch chip's INT line, soldered to ground by a routing
// mistake: never an output here.
#pragma once

#define BOARD_NAME "cyd-3.5-cap"

#define BOARD_LCD_SCK 14
#define BOARD_LCD_MOSI 13
#define BOARD_LCD_MISO 12
#define BOARD_LCD_CS 15
#define BOARD_LCD_DC 2
#define BOARD_LCD_BL 27                // PWM, active high
#define BOARD_LCD_HZ (40 * 1000 * 1000)
#define PANEL_W 320                    // the glass as mounted: portrait
#define PANEL_H 480
#define PANEL_MIRROR_X 1               // scans its X the other way (a mirror image with no mirroring, 2026-10-06)
#define BOARD_BGR_DEFAULT true         // red showed as yellow with RGB (2026-10-06)
#define BOARD_INVERT_DEFAULT false     // CMYK with it on (2026-10-06)
#define BOARD_FONT_DEFAULT 18

#define BOARD_TP_SDA 33
#define BOARD_TP_SCL 32
#define BOARD_TP_RST 25
#define BOARD_TP_INT 21                // soldered to GND on this board: the chip is polled
#define BOARD_Z_SOLID 1                // a capacitive glass has no pressure: every point is believed

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

#define SPEAKER_RATE 22050             // what /health says in `plays`; Sapphire fits the sound to it

#define BOARD_FREE_PINS {22, 35}       // on the connectors; 35 is input only. The console's `i2c` takes no other
