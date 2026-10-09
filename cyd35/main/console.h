// console.h - the setup line over the CH340 serial port. One command a line,
// one ">> " JSON line back. setup_board.py is the other end.
//   show                     what it knows about itself (no secrets)
//   keys                     the two keys
//   setup {json}             name, wifi_ssid, wifi_password, key, voice_key, sapphire, cert; saves and restarts
//   test                     the test card on the screen; touches print TOUCH x y
//   touch                    the touch chip through its driver for 10 s; RAW x y n
//   gt                       the GT911's status register raw for 10 s
//   i2c <sda> <scl> [rst]    who answers on those pins
//   screen invert 0|1        the panel's colors, kept
//   screen flip 0|1          180 degrees, kept
//   screen bright 10..100    the backlight, kept
//   light r g b              the LED, 6 seconds
//   restart, erase
#pragma once

void console_start(void);
