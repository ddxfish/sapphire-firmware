// board.h - the board under the program: one of Sapphire's ESP32-S3 satellites, chosen when the
// program is built (`build.sh pendant` or `build.sh ai-speaker-mic`: BOARD_PINS names the board's
// own header in boards/, and boards/<name>.c with boards/<name>/ holds its chips). Everything above
// this file is the same program, built twice (2026-11-12: the two trees had drifted 4130 lines
// apart in ten days, and every lesson had to be found twice).
//
// Every function here exists on every board. What a board lacks it answers honestly: no rail, no
// fill light, no row gap, no keys. The BOARD_HAS_* and BOARD_CAN_* words say so at build time, so a
// page, a route or a /health key for something the board has not is not built at all.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#include BOARD_PINS                    // boards/pendant.h or boards/ai-speaker-mic.h: pins, sizes, what it has

#ifndef BOARD_LEDS
#define BOARD_LEDS 1                   // status lights: one (the pendant's RGB) or a ring (the speaker's seven)
#endif
#ifndef BOARD_SPEAKER_RATE_ALT
#define BOARD_SPEAKER_RATE_ALT BOARD_SPEAKER_RATE   // a second rate the speaker plays live (the pendant: 16 kHz beside 24)
#endif
#ifndef BOARD_SOFT_AEC
#define BOARD_SOFT_AEC 0               // a codec loopback (the speaker): the canceller's knobs are the codec's, not ours
#endif
#ifndef BOARD_UART_CONSOLE
#define BOARD_UART_CONSOLE 0           // the console on UART0 (a line on it wakes a sleeping board)
#endif
#ifndef BOARD_KEYS
#define BOARD_KEYS 0                   // keys besides BOOT (the speaker's Key1-3 on its expander)
#endif
#ifndef BOARD_CAN_SLEEP
#define BOARD_CAN_SLEEP 0              // a switched rail and a BOOT wake: light sleep, the dead-battery guard (power.c)
#endif
#ifndef BOARD_HAS_RAIL
#define BOARD_HAS_RAIL 0               // a switched rail under the screen, the camera, the sound
#endif
#ifndef BOARD_HAS_GAP
#define BOARD_HAS_GAP 0                // a glass shorter than its chip's frame memory: the row gap moves with a flip
#endif
#ifndef BOARD_HAS_FILL
#define BOARD_HAS_FILL 0               // a white light beside the lens
#endif

// --- the board itself ----------------------------------------------------------------
bool board_init(int rate);             // I2C, the expander, the sound chips and their I2S; the speaker at `rate`
void board_console_io(void);           // the console's line in and out: UART0 (pendant) or USB-Serial-JTAG (speaker)
const char *board_trouble(void);       // "" or why board_init stopped
int board_i2c_scan(cJSON *list);       // the 7 bit addresses that answer; -1 without a bus
void board_i2c_counts(int *errors, int *resets, int *held);   // failed touch polls, touch rests, clock pulses SDA needed at boot
bool board_boot_down(void);            // the BOOT key, now
int board_keys(void);                  // bit 0..BOARD_KEYS-1 down; 0 with no keys; -1 = not known just now

struct button_def;
const struct button_def *board_buttons(size_t *n);   // BOOT, its keys, then the glass's places (buttons.h); main.c feeds BOOT and the keys

// --- sound ------------------------------------------------------------------------------
board_audio_t *board_audio(void);      // its shape is the board's own (boards/<name>.h); only boards/<name>/audio.c reads it
void board_amplifier(bool on);
bool board_speaker_rate(int rate);     // the speaker's clock, live; false for a rate this board cannot play
int board_speaker_check(void);         // the sound chips read back and put right: -1 unreadable, 0 fine, 1 healed (the expander: the glass is healed too), 2 a codec alone
int board_speaker_heals(const char **last);

// --- power ------------------------------------------------------------------------------
bool board_rail(bool on);              // the switched rail (screen, camera, mic, amplifier, card); false: no rail, or the write failed
bool board_rail_on(void);              // true on a board with no rail: its parts are always powered
bool board_dark(void);                 // before board_init, at a dead-battery boot: every output the board can turn off, off

// --- lights -----------------------------------------------------------------------------
bool board_leds_start(void);           // after board_init: false = no light
void board_led_set(int i, uint8_t r, uint8_t g, uint8_t b);   // 0 .. BOARD_LEDS-1, shown at board_leds_show
void board_leds_show(void);
void board_leds_clear(void);           // all dark, at once
void board_fill_light(bool on);        // the white light beside the lens; nothing without one
bool board_fill_light_on(void);

// --- the glass --------------------------------------------------------------------------
typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    esp_lcd_touch_handle_t touch;      // NULL when the chip did not answer
    bool up;
} board_glass_t;

bool board_glass_init(void);           // after board_init: SPI bus, panel, touch chip. false = no glass
bool board_glass_wake(void);           // the panel and the touch chip set up again (after the rail, or an expander heal)
board_glass_t *board_glass(void);
bool board_touch_read(int *x, int *y);  // one poll: true while a finger is down. Never blocks past 30 ms
int board_touch_reg(int reg);          // one register of the touch chip, raw; -1 when it did not answer
bool board_touch_int(void);            // the chip's INT line, low while it has something; false where it reaches no GPIO
void board_backlight(int percent);     // 0 = dark
void board_glass_orient(bool invert, bool flip, bool mirror);   // colors, 180 degrees, left-right; live, the touch follows
void board_glass_gap(int y);           // bench: the row gap, applied now (nothing without BOARD_HAS_GAP)
int board_glass_gap_now(void);

// --- the card and the eye ---------------------------------------------------------------
bool board_sd_select(bool high);       // a card select line on an expander; true where there is none to drive
void board_camera_power(bool on);      // a power-down line; nothing where the sensor has none

// --- the battery ----------------------------------------------------------------------------
typedef struct { int raw, pad_mv, mv, spread_mv, percent; bool battery; } board_battery_t;
bool board_battery_read(board_battery_t *out);   // false when the board has no sense line or the ADC did not start
