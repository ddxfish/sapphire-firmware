// console.h - the setup line over the USB plug. One command a line, one
// ">> " JSON line back. Sapphire's flasher and setup_board.py are the other end.
//   show                     what it knows about itself (no secrets), the card and its backups
//   keys                     the two keys
//   scan                     the WiFi networks around
//   setup {json}             name, wifi_ssid, wifi_password, key, voice_key, sapphire, cert; saves and restarts
//   test                     the test card on the screen, 20 s
//   card [MB] [keep]        a test file through the path a backup takes, timed, then removed
//   screen flip 0|1          180 degrees, kept; restarts
//   screen bright 10..100    the backlight, kept
//   light r g b              the LED, 6 seconds
//   restart, erase
#pragma once

void console_start(void);
