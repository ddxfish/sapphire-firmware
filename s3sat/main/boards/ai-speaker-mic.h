// NAME: AI speaker and mic (Waveshare ESP32-S3 audio board)
// boards/ai-speaker-mic.h - the Waveshare ESP32-S3-AUDIO-Board: pins, I2C, the two codec
// chips and their I2S bus (the functions are board.h's, boards/ai-speaker-mic.c answers them).
//
// Verified 2026-09-29 against Xiaozhi's board file and Waveshare's schematic:
//   ES8311 (speaker DAC) at I2C 0x18, ES7210 (mic ADC) at 0x40, TCA9555 at 0x20
//   I2S: MCLK 12, BCLK 13, WS 14, DIN 15 (from ES7210), DOUT 16 (to ES8311)
//   speaker amplifier on/off = TCA9555 pin 8; the speaker's own signal is fed
//   back into the ES7210 as MIC3, which is what echo cancelling needs
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_codec_dev.h"

#define BOARD_NAME "waveshare-s3-audio"
#define BOARD_DEFAULT_NAME "satellite"
#define BOARD_LED_GPIO 38
#define BOARD_LEDS 7                   // the WS2812 ring
#define BOARD_BOOT_BUTTON 0
// No switched rail (the panel, the codecs, the ring and the camera are powered while the chip is), no BOOT
// wake: it never sleeps (BOARD_CAN_SLEEP 0). The console is USB-Serial-JTAG (BOARD_UART_CONSOLE 0).
#define BOARD_SPEAKER_RATE 16000       // ONE I2S clock for the speaker and the mics, and the mics' front end needs 16 kHz
#define MIC_GAIN_X2_DEFAULT 60         // 30 dB, the value the first firmware hard-coded
#define MIC_GAIN_X2_MAX 75
#define STREAM_FPS_DEFAULT 12          // the panel bus is 40 MHz; changed regions only cross it on her stream
#define BOARD_AV_W 320                 // her stream's face fills the whole glass (1.0.0; the hub crops or bars her into it)
#define BOARD_AV_H 240
bool board_out_volume(int level);     // the ES8311's volume, written until it reads back (boards/ai-speaker-mic.c)
#define BOARD_SPK_DESC_FRAMES 240      // one DMA descriptor of the I2S out (i2s_init): the air is this far behind a full ring (avatar.c LEAD_MS)
#define BOARD_SPK_DESC_NUM 6
#define BOARD_CAM_SCCB_PORT 0          // the camera's control channel rides the shared I2C bus
#define BOARD_CAM_JPEG 1               // the OV sensors make their own JPEG

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

// The three keys (schematic 1.1, read 2026-10-10): Key1, Key2, Key3 on the
// expander's EXIO9, 10, 11, each with 10K to 3V3 and pressed to ground. The
// expander's interrupt line is not connected (R22 not fitted), so they are polled.
#define BOARD_KEYS 3

// The TF slot (Waveshare wiki, read 2026-10-05): CLK/SCK GPIO 40, CMD/MOSI
// GPIO 42, D0/MISO GPIO 41, D1 and D2 not connected, D3/CS on the expander
// (EXIO3). One data line, so SDMMC 1-bit over the GPIO matrix; D3 held high
// by the expander keeps the card in SD mode. Never SDMMC_SLOT_CONFIG_DEFAULT:
// its S3 defaults sit on the I2S pins.
#define BOARD_SD_CLK 40
#define BOARD_SD_CMD 42
#define BOARD_SD_D0 41
#define BOARD_STORAGE_CHECKS           // storage.c: read every backup back, keep its hash, check them all at start (0.7.0)

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
#define BOARD_LCD_HZ (40 * 1000 * 1000)     // 40: a 60 MHz trial (1.0.0, 2026-11-12) left a tile slide at 45-95 ms a frame, no better than 40's 45-70 (the draw in PSRAM is the cost, not the bus), and a long ribbon likes the slower clock. NOT 80: the DMA reads PSRAM, and at 80 MHz it outran the PSRAM (data lost, "io tx param failed"), then esp_lcd's in-flight count leaked and the next paint waited for ever (0.7.9, 2026-10-21)
#define BOARD_PANEL_W 240              // the panel's own axes
#define BOARD_PANEL_H 320
#define BOARD_W 320                    // the glass as it is used: landscape, the plug on a side (2026-10-07)
#define BOARD_H 240
#define BOARD_DRAW_LINES 240            // LVGL's draw buffers: the whole glass, twice, in PSRAM (the pendant's recipe for a smooth slide; 16-line strips made the tile slide "meh", 2026-11-12). 40 MHz: the DMA keeps up with PSRAM


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

// --- the battery (0.7.5): a 3.7 V cell on the MX1.25 plug, an ETA6098 charger. Schematic
// 1.1: BAT -> 200K -> BAT_ADC -> 100K -> GND, so the pin sees a third of the cell, taken
// before the battery switch. BAT_ADC reaches the chip only through solder jumper R78: one
// side is GPIO6 (ADC1 channel 5, free here: LCD_SDA3 is a QSPI panel line and the ST7789 is
// plain SPI), the other, through R53, GPIO1 in place of the camera's HREF (Waveshare's
// documented way: no camera, never). Read on GPIO6. A board whose R78 is open reads a
// floating pin, out of a cell's range or jumping, and claims no battery.
#define BOARD_BATTERY_GPIO 6
#define BOARD_BATTERY_DIVIDER 3
// board_battery_t (board.h): raw = the ADC's mean of 16 reads; pad_mv at the pin by the chip's curve; mv = the
// cell; spread_mv = highest read less lowest (a floating pin jumps); percent from a cell's resting curve.
