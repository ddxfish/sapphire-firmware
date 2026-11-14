// NAME: Sapphire pendant (Elecrow AI Camera board)
// boards/pendant.h - the Elecrow AI Camera board (ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM):
// Sapphire's pendant. Pins and the port expander's map read from Elecrow's Eagle
// schematic V1.0 (2026-11-02); the I2S shapes are the ones Elecrow's own program uses.
//
//   USB-C -> CH340K -> UART0 (GPIO 43/44): the console. There is no native USB
//     (GPIO 19 and 20 are camera data), so the console is the UART, as on the CYD.
//   I2C0 (SDA 16, SCL 15): the CST816S touch chip at 0x15, the AW9523 expander at 0x5A.
//   AW9523: P1_0 PWR_EN gates the switched 3V3 rail (screen, camera, mic, amplifier,
//     card, both LEDs: everything but this chip, the expander and the CH340);
//     P1_1 the white fill light; P1_3 sinks the backlight's cathode (LED mode =
//     the dimmer); P1_4 the NS4168 amplifier's power through a P-FET, LOW = on (V1.0,
//     the board on the bench; V1.1 moved it to P1_5 and put the touch reset on P1_4: BOARD_REV11);
//     P0_4/5/6 the RGB status LED's G/R/B, common anode, LED mode.
//   Screen: an ST7789-class 240x284 on SPI2 (SCK 14, MOSI 13, CS 10, DC 9); its
//     reset is an RC to the rail and a diode to the chip's EN: no GPIO.
//   Speaker: NS4168, I2S (BCLK 5, LRCLK 4, DATA 3). No codec chip: volume is ours.
//   Mic: a PDM MEMS (CLK 1, DATA 2, L/R to ground). I2S0 is the one port that takes PDM.
//   Camera: GC2145 (or OV2640 by a resistor) on DVP, SCCB on its own pins (40/41),
//     reset tied to EN, no power-down pin: it idles powered unless the rail is cut.
//   Card: SPI (SCK 7, MISO 6, MOSI 8), chip select tied: alone on its bus.
//   Battery: TP4059 charger, JST SH 1.0 plug. NO sense line reaches the chip.
//   Buttons: BOOT (GPIO 0, the side key), RESET (EN).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "driver/i2s_common.h"

#define BOARD_NAME "elecrow-ai-camera"
#define BOARD_DEFAULT_NAME "pendant"
#define BOARD_BOOT_BUTTON 0
#define BOARD_LEDS 1                   // the RGB status LED on the expander
#define BOARD_CAN_SLEEP 1              // the rail and BOOT on GPIO 0: light sleep, the dead-battery guard
#define BOARD_HAS_RAIL 1
#define BOARD_HAS_GAP 1
#define BOARD_HAS_FILL 1
#define BOARD_SOFT_AEC 1               // the echo reference is made in software (audio.c): afe_mode, aec_filter, delay are real knobs
#define BOARD_UART_CONSOLE 1           // UART0 through the CH340K: a line on it wakes the board too
#define BOARD_SPEAKER_RATE 24000       // her voice: Kokoro's own rate; the amplifier's clock is its own (16000 too, live)
#define BOARD_SPEAKER_RATE_ALT 16000
#define MIC_GAIN_X2_DEFAULT 24         // 12 dB, digital: a PDM mic has no analog gain, and 30 dB clipped
#define MIC_GAIN_X2_MAX 60
#define TOUCH_DY_DEFAULT 10            // the chip reads a few mm high; Elecrow adds 10
#define BOARD_AV_W 240                 // the box her stream's face fills (/health screen.av_w, av_h): the whole glass, overscanned
#define BOARD_AV_H 288                 // 284 rows on the 16 px grid: 2 px under each curved edge (1.0.0; was a 240 square with 22 px bands)

#define BOARD_I2C_SDA 16
#define BOARD_I2C_SCL 15
#define BOARD_I2C_HZ 100000            // 400 kHz read back garbage from the touch chip and a bit-flipped expander id (2026-11-02): the FPC and 10K pull-ups
#define BOARD_TP_RST 12
#define BOARD_TP_INT 11

// --- sound ---------------------------------------------------------------------------
#define BOARD_MIC_CLK 1
#define BOARD_MIC_DATA 2
#define BOARD_SPK_BCLK 5
#define BOARD_SPK_WS 4
#define BOARD_SPK_DOUT 3
// What the audio front end is fed: the mic, and the speaker's own signal made here
// (audio.c keeps what it plays and hands it back, delayed): "MR", two channels.
#define BOARD_CHANNELS 2
#define BOARD_AFE_FORMAT "MR"
#define BOARD_SLOTS 2                  // audio_selftest_t's arrays: mic, reference

typedef struct {
    i2s_chan_handle_t rx;              // the mic: 16 kHz mono 16 bit, PDM decoded by the chip
    i2s_chan_handle_t tx;              // the amplifier: 16 kHz stereo 16 bit, both slots the same
} board_audio_t;

uint32_t board_mic_dropped(void);      // mic samples the DMA ring lost since boot (the feed task was late)
#define BOARD_SPK_DESC_FRAMES 160      // one DMA descriptor of the amplifier: a first write plays when the current one ends
#define BOARD_SPK_DESC_NUM 12          // descriptors in the ring: the air is this far behind the feed when the ring is full (avatar.c LEAD_MS)
void board_spk_sent(uint64_t *frames, int64_t *at_us);   // frames the amplifier's DMA has played since boot (sound or silence) and when the last descriptor ended
// the expander's other pins: the rail (P1_0: screen, camera, mic, amplifier, card, LEDs), the fill light,
// the status LED: board.h's board_rail, board_fill_light, board_led_set (one light)

// --- the glass ---------------------------------------------------------------------
#define BOARD_LCD_SCK 14
#define BOARD_LCD_MOSI 13
#define BOARD_LCD_CS 10
#define BOARD_LCD_DC 9
#define BOARD_LCD_HZ (60 * 1000 * 1000)     // Elecrow's own 60 (0.1.39: a face frame's blit held the glass lock 50-100 ms at 40); 80 outran the PSRAM draw buffers on the speaker board
#define BOARD_PANEL_W 240
#define BOARD_PANEL_H 284
#define BOARD_PANEL_ROWS 320           // the ST7789's frame memory: 36 rows are never on this glass; turned 180 they are at the other edge (the gap moves)
#define BOARD_GAP_UP 0                 // the row the glass begins at in that memory, unturned (bench: POST /screen/gap?y=N finds it by eye)
#define BOARD_GAP_TURNED (BOARD_PANEL_ROWS - BOARD_PANEL_H)   // and turned 180
#define BOARD_W 240                    // portrait, as worn
#define BOARD_H 284
#define BOARD_DRAW_LINES 284           // whole-frame draw buffers, two, in PSRAM: Elecrow's recipe for the feel
#define BOARD_TOUCH_MIRROR_X 0         // a swipe pulled the page the wrong way with X mirrored (2026-11-02): the chip scans X as the panel does
#define BOARD_TOUCH_MIRROR_Y 1         // Elecrow turns the touch 180 degrees and then mirrors X back: net, Y is mirrored (their y + 10 is left out)


// --- the card: SPI, alone on SPI3, no chip select line (storage.c) ---------------
#define BOARD_SD_SPI
#define BOARD_SD_HOST SPI3_HOST
#define BOARD_SD_CS GPIO_NUM_NC
#define BOARD_SD_SCK 7
#define BOARD_SD_MISO 6
#define BOARD_SD_MOSI 8
#define BOARD_STORAGE_CHECKS

// --- the eye: DVP, SCCB on its own pins ---------------------------------------------
#define BOARD_CAM_XCLK 39
#define BOARD_CAM_SIOC 40
#define BOARD_CAM_SIOD 41
#define BOARD_CAM_VSYNC 42
#define BOARD_CAM_HREF 45
#define BOARD_CAM_PCLK 48
#define BOARD_CAM_D0 20
#define BOARD_CAM_D1 18
#define BOARD_CAM_D2 17
#define BOARD_CAM_D3 19
#define BOARD_CAM_D4 21
#define BOARD_CAM_D5 47
#define BOARD_CAM_D6 38
#define BOARD_CAM_D7 46

// --- the battery: there is no sense line on this board: board_battery_read answers false,
// and /health claims no sensors. -------------
#define BOARD_BATTERY_GPIO (-1)
#define BOARD_CAM_SCCB_PORT 1          // the camera's own two pins, a second I2C port
#define BOARD_CAM_JPEG 0               // the GC2145 makes no JPEG: the S3 encodes
