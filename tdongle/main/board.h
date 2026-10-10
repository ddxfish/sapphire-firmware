// board.h - LilyGO T-Dongle-S3: an ESP32-S3 (no PSRAM, 16 MB flash) in a USB
// stick. A 0.96" ST7735S 160x80 screen, one APA102 LED, the BOOT button, and
// a TF card slot inside the USB plug: the card cannot come out while the
// stick is plugged in.
//
// Pins from LilyGO's pin sheet; the screen's start-up sequence and its place
// inside the controller's memory are the ones the old gadget firmware ran on
// this very stick.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#define BOARD_NAME "t-dongle-s3"

#define BOARD_LCD_SCK 5
#define BOARD_LCD_MOSI 3
#define BOARD_LCD_CS 4
#define BOARD_LCD_DC 2
#define BOARD_LCD_RST 1
#define BOARD_LCD_BL 38                // PWM, on when LOW
#define BOARD_LCD_HZ (20 * 1000 * 1000)
#define BOARD_W 160                    // landscape: the plug to the left
#define BOARD_H 80
#define BOARD_LCD_GAP_X 1              // where the glass sits in the controller's memory
#define BOARD_LCD_GAP_Y 26

#define BOARD_LED_DATA 40              // APA102
#define BOARD_LED_CLOCK 39
#define BOARD_BOOT_BUTTON 0            // low when pressed

#define BOARD_SD_CLK 12                // the card, SDMMC. One data line: slower than four and
#define BOARD_SD_CMD 16                // still faster than WiFi, with three fewer lines to go wrong
#define BOARD_SD_D0 14
#define BOARD_SD_D3 18                 // held high: the card stays in SD mode
#define BOARD_STORAGE_CAPS MALLOC_CAP_DEFAULT   // no PSRAM: the write buffer is internal
#define BOARD_STORAGE_CHUNK (16 * 1024)
#define BOARD_STORAGE_CHECKS           // storage.c: read every backup back, keep its hash, check them all at start

#define BOARD_DRAW_LINES 40            // LVGL's draw buffer: half the screen, twice = 25 KB

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    bool flip;                         // 180 degrees: the plug to the right
} board_t;

bool board_init(void);                 // SPI, panel, backlight, LED pins, button
board_t *board(void);
void board_backlight(int percent);     // 0 = dark
void board_led(uint8_t r, uint8_t g, uint8_t b);
bool board_button(void);               // true while BOOT is held
void board_set_flip(bool on);          // kept in flash; takes effect at the next start
bool board_sd_select(bool on);         // the card's D3 line high (storage.c asks before it mounts)
