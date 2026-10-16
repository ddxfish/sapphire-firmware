// NAME: CYD 2.8" reader (ESP32-2432S028R, resistive touch)
// boards/cyd28.h - the 2.8" "Cheap Yellow Display", resistive kind
// (ESP32-2432S028R): ESP32-D0WD in a WROOM module (no PSRAM, 4 MB flash), a
// 2.8" 240x320 panel on HSPI (an ILI9341, or an ST7789 on the two-USB
// revision: the chip is asked at boot), an XPT2046 touch chip on wires of its
// own, one RGB LED, a light sensor, an amplifier on the DAC pin, a TF slot on
// VSPI, the BOOT button.
//
// Pins from witnessmenow's PINS.md and the mischianti schematic (2026-10-15).
// Two SPI hosts, three buses: the panel holds HSPI, the card holds VSPI, so
// the touch chip is driven by hand on its pins (boards/cyd28.c). Its MISO
// (39) and IRQ (36) are input-only pins; ESP32 errata 3.11 says both glitch
// low for ~80 ns when ADC1 samples (the light sensor does), so IRQ is not
// consulted and the chip's pressure reading says whether anyone is touching.
#pragma once

#define BOARD_NAME "cyd-2.8-res"

#define BOARD_LCD_SCK 14
#define BOARD_LCD_MOSI 13
#define BOARD_LCD_MISO 12
#define BOARD_LCD_CS 15
#define BOARD_LCD_DC 2
#define BOARD_LCD_BL 21                // PWM, active high
#define BOARD_LCD_HZ (40 * 1000 * 1000)
#define BOARD_LCD_READ_HZ (5 * 1000 * 1000)   // a panel answers its ID only at a slow clock
#define PANEL_W 240                    // the glass as mounted: portrait
#define PANEL_H 320
#define PANEL_MIRROR_X 0               // scans its X the standard way
#define BOARD_BGR_DEFAULT false        // red first (blue drew orange as BGR on the first board)
#define BOARD_INVERT_DEFAULT false     // whatever its chip; `screen invert 1` for a glass that wants it
#define BOARD_FONT_DEFAULT 16

#define BOARD_TP_CLK 25
#define BOARD_TP_MOSI 32
#define BOARD_TP_MISO 39               // input only
#define BOARD_TP_CS 33
#define BOARD_TP_IRQ 36                // input only; low while touched, not always
#define BOARD_Z_SOLID 700              // pressure at which a point is believed (touched from 400)

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

#define BOARD_FREE_PINS {22, 27}       // on the CN1 connector; the console's `i2c` takes no other
