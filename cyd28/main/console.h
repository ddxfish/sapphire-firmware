// console.h - the setup line over the CH340 serial port. One command a line,
// one ">> " JSON line back. setup_board.py is the other end.
//   show, scan                     what it knows about itself (no secrets): the panel it found, the touch, the calibration
//   keys                     the two keys
//   setup {json}             name, wifi_ssid, wifi_password, key, voice_key, sapphire, cert; saves and restarts
//   test                     the test card on the screen; touches print TOUCH x y
//   touch                    the touch chip raw for 10 s: RAW x y z
//   touch cal                three crosses to tap; CAL lines; the result is kept
//   screen invert 0|1        the panel's colors, kept
//   screen flip 0|1          180 degrees, kept
//   screen mirror 0|1        the X axis the other way, kept
//   screen bgr 0|1           blue first on the wire (red and blue swapped on the glass), kept; restarts
//   screen bright 10..100    the backlight, kept
//   screen font 14..24       the chat font (14, 16, 18, 20, 22, 24), kept; restarts
//   screen home <page>       the page the board opens on: chat, backups, status, board, actions; kept
//   screen panel auto|ili9341|st7789   which chip drives the glass, kept; restarts
//   i2c <sda> <scl> [rst]    who answers on those pins (a sensor on CN1)
//   light r g b              the LED, 6 seconds
//   restart, erase
#pragma once

void console_start(void);
