// console.h - the USB setup line. One command per line on the USB serial
// port, every answer on one line that starts with ">> ". The setup script
// (setup_board.py) is the other end; a person with a terminal works too.
//
//   show                          what is set, never a password or key
//   keys                          the two keys, for Settings > Devices
//   setup {"name": ..., ...}      set everything at once, then restart
//   selftest                      a tone through the speaker, heard by the mic
//   tasks                         where the time goes: each task's share of a core, and the idle per core
//   i2c                           who answers on the I2C bus
//   heap                          internal and PSRAM room, and the lowest internal ever
//   test                          the glass's test card: frame, TOP, color bars, taps echoed
//   screen invert|flip 0|1        the glass's colors and turn, kept; screen bright 0..100
//   restart
//   erase                         forget every setting
#pragma once

void console_start(void);
