// board.h - the Waveshare ESP32-S3-AUDIO-Board: pins, I2C, the two codec
// chips and their I2S bus. Another board gets its own board.c with the same
// functions and the same names below.
//
// Verified 2026-09-29 against Xiaozhi's board file and Waveshare's schematic:
//   ES8311 (speaker DAC) at I2C 0x18, ES7210 (mic ADC) at 0x40, TCA9555 at 0x20
//   I2S: MCLK 12, BCLK 13, WS 14, DIN 15 (from ES7210), DOUT 16 (to ES8311)
//   speaker amplifier on/off = TCA9555 pin 8; the speaker's own signal is fed
//   back into the ES7210 as MIC3, which is what echo cancelling needs
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_codec_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#define BOARD_NAME "waveshare-s3-audio"
#define BOARD_LED_GPIO 38
#define BOARD_LEDS 7
#define BOARD_BOOT_BUTTON 0

// The ES7210 hands its four inputs over as four TDM slots, in the order
// MIC1, MIC3, MIC2, MIC4. Measured on 2026-09-29 with `setup_board.py test`:
// in quiet the slots read [22, 0, 23, 0], during a tone from the speaker
// [707, 73, 737, 0]. Slot 1 hears no room and only the speaker: it is the
// speaker's own signal (MIC3), the echo reference. Slots 0 and 2 are the two
// mics. Slot 3 is not connected.
//   M = a microphone, R = the speaker's own signal
// Only slots 0 and 1 are taken from the codec (BOARD_TAKE), so the DMA and
// the AFE carry two channels, not four. Taking slot 2 as well and saying
// "MRM" would use both mics (more CPU, a little better).
#define BOARD_SLOTS 4                  // TDM slots on the wire
#define BOARD_TAKE 0x3                 // the slots read: 0 (mic 1) and 1 (the echo)
#define BOARD_CHANNELS 2               // popcount of BOARD_TAKE
#define BOARD_AFE_FORMAT "MR"

typedef struct {
    esp_codec_dev_handle_t in;         // the mics, 4 slots of 16 bit at 16 kHz
    esp_codec_dev_handle_t out;        // the speaker, mono 16 bit at 16 kHz
} board_audio_t;

bool board_init(int rate);             // I2C, expander (amplifier on), I2S, both codecs
board_audio_t *board_audio(void);
void board_amplifier(bool on);
// The three keys (schematic 1.1, read 2026-10-10): Key1, Key2, Key3 on the
// expander's EXIO9, 10, 11, each with 10K to 3V3 and pressed to ground. The
// expander's interrupt line is not connected (R22 not fitted), so they are polled.
#define BOARD_KEYS 3
int board_keys(void);                  // bit 0..2 set = Key1..3 down; -1 = not known just now
int board_i2c_scan(cJSON *list);       // the 7 bit addresses that answer, as numbers; -1 without a bus
const char *board_trouble(void);       // "" or why board_init stopped, in words
void board_i2c_counts(int *errors, int *resets, int *held);   // failed touch polls, touch rests, and the clock pulses SDA needed at boot

// The TF slot (Waveshare wiki, read 2026-10-05): CLK/SCK GPIO 40, CMD/MOSI
// GPIO 42, D0/MISO GPIO 41, D1 and D2 not connected, D3/CS on the expander
// (EXIO3). One data line, so SDMMC 1-bit over the GPIO matrix; D3 held high
// by the expander keeps the card in SD mode. Never SDMMC_SLOT_CONFIG_DEFAULT:
// its S3 defaults sit on the I2S pins.
#define BOARD_SD_CLK 40
#define BOARD_SD_CMD 42
#define BOARD_SD_D0 41
bool board_sd_select(bool high);       // D3/CS through the expander; false when the expander is not up

// --- the glass (0.4.0): Waveshare's 2inch Capacitive Touch LCD on the 18 pin plug ---
// ST7789T3, 240x320, plain SPI; a CST816D touch chip on the shared I2C bus.
// Pins from Waveshare's wiki and Xiaozhi's board file: CS 3, SCK 4, backlight
// 5, DC 7, MISO 8 (the 2 inch has none), MOSI 9. LCD reset is EXIO0 and touch
// reset EXIO1, both pulsed in expander_init. TP_INT is EXIO2, not a GPIO, so
// the touch chip is polled. Xiaozhi runs this panel RGB with inverted colors.
#define BOARD_LCD_CS 3
#define BOARD_LCD_SCK 4
#define BOARD_LCD_BL 5
#define BOARD_LCD_DC 7
#define BOARD_LCD_MOSI 9
#define BOARD_LCD_HZ (40 * 1000 * 1000)
#define BOARD_PANEL_W 240              // the panel's own axes
#define BOARD_PANEL_H 320
#define BOARD_W 320                    // the glass as it is used: landscape, the plug on a side (2026-10-07)
#define BOARD_H 240
#define BOARD_DRAW_LINES 16             // LVGL's draw buffers: 240 x 16 x 2 bytes, twice, in PSRAM (DMA reads it there); 64-byte multiples

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    esp_lcd_touch_handle_t touch;      // NULL when the chip did not answer
    bool up;
} board_glass_t;

bool board_glass_init(void);           // after board_init: SPI bus, panel, touch chip, backlight. false = no glass
board_glass_t *board_glass(void);
bool board_touch_read(int *x, int *y);  // one poll: true while a finger is down. Never blocks past 30 ms, never aborts
void board_backlight(int percent);     // 0 = dark
void board_glass_orient(bool invert, bool flip);   // the panel's colors, landscape, and 180 degrees; live

// --- the eye: the 24 pin DVP plug (Waveshare's wiki, Xiaozhi's board file) ---
// PCLK and XCLK sit on GPIO 44/43 while EXIO6 is high (low moves them to 19/20
// and kills native USB: never). PWDN is EXIO5: high = powered down, the rest
// state. SCCB is the shared I2C bus. HREF shares GPIO 1 with the battery ADC jumper.
#define BOARD_CAM_XCLK 43
#define BOARD_CAM_PCLK 44
#define BOARD_CAM_VSYNC 21
#define BOARD_CAM_HREF 1
#define BOARD_CAM_D0 2
#define BOARD_CAM_D1 17
#define BOARD_CAM_D2 18
#define BOARD_CAM_D3 39
#define BOARD_CAM_D4 45
#define BOARD_CAM_D5 46
#define BOARD_CAM_D6 47
#define BOARD_CAM_D7 48
void board_camera_power(bool on);      // EXIO5; false when the expander is not up
