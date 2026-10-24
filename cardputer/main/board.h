// board.h - M5Stack Cardputer ADV (Stamp-S3A = ESP32-S3FN8, no PSRAM).
//
// Shared I2C bus GPIO8 (SDA) / GPIO9 (SCL), 100-400 kHz:
//   TCA8418 keyboard controller @0x34 (interrupt GPIO11)   [P2 keyboard, not yet]
//   ES8311 audio codec          @0x18   (mic ADC + speaker DAC, full duplex)
//   BMI270 IMU                  @0x68                        [later]
// ES8311 I2S (one bus, full duplex): BCK 41, WS 43, DSDIN/speaker 42,
//   ASDOUT/mic 46; MCLK unused (the codec derives it from BCLK).
//
// NOTE (why a probe): ESP-IDF issue #18621 — the ES8311 mic returns silent/
// constant samples on the v5.5.x line (we build on v5.5.5). The speaker is fine.
// `mic` on the console reads the ADC so we can see whether that bug bites us here.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BOARD_I2C_SDA   8
#define BOARD_I2C_SCL   9
#define BOARD_KBD_INT   11
#define BOARD_TCA8418   0x34
#define BOARD_I2S_BCLK  41
#define BOARD_I2S_WS    43
#define BOARD_I2S_DOUT  42      // DSDIN -> speaker
#define BOARD_I2S_DIN   46      // ASDOUT <- mic

// microSD (TF) card on its own SPI bus (SPI2; the screen owns SPI3). FAT32,
// mounted at /sdcard. The standard Cardputer SD pinout.
#define BOARD_SD_SCK    40
#define BOARD_SD_MISO   39
#define BOARD_SD_MOSI   14
#define BOARD_SD_CS     12

#define AUDIO_RATE      16000   // mic capture (and, later, speaker) sample rate

bool board_init(int rate);                       // I2C bus + I2S + ES8311 (mic+speaker) + keyboard
int  board_mic_read(int16_t *buf, int samples);  // blocking read of `samples` mono frames; <0 on error
int  board_speaker_write(const int16_t *pcm, int samples);  // blocking play of `samples` mono frames; <0 on error
void board_speaker_vol(int percent);             // 0..100
const char *board_trouble(void);                 // "" or why board_init stopped

// TCA8418 keypad on the shared I2C bus. board_kbd_read pops one event: `key` is
// the raw TCA8418 key number (row*10 + col + 1, unmapped), `down` = pressed.
bool board_kbd_init(void);                        // idempotent; false if the chip is absent
bool board_kbd_read(int *key, bool *down);        // false when no event is waiting
void board_kbd_hold(bool on);                     // true = a console dump owns the FIFO; PTT must not drain it
bool board_kbd_busy(void);                         // PTT checks this before reading the keyboard
