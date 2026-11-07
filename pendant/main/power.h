// power.h - sleep, and waking from it.
//
// Asleep, the switched rail is off (screen, camera, mic, amplifier, card, LEDs all
// unpowered), WiFi is off and the chip is in light sleep with everything in RAM. BOOT
// wakes it (a line on the console does too), the rail comes back, the panel and the
// touch chip are set up again, WiFi rejoins: a few seconds, no reboot. Light sleep and
// not deep sleep on purpose: a deep-sleep wake runs the ROM, which reads GPIO 0 as the
// download strap while the very key that woke the board is still held.
//
// The board goes to sleep by itself after settings.sleep_after_min of nothing (0 = never),
// by a long press of BOOT, by the Settings app, or by Sapphire (POST /power?action=sleep).
#pragma once
#include <stdbool.h>

void power_sleep_soon(void);           // from any task: main's watch task does it within a second
void power_restart_soon(void);         // the same for a restart (settings are flushed first, from an internal stack)
bool power_asleep(void);
bool power_no_sleep(void);             // the last reset was a brownout: sleep is refused until a power cycle
bool power_sleep_pending(void);
void power_tick(void);                 // every second from the watch task (internal stack): the timer, the asks
int power_awake_s(void);               // since boot or the last wake
int power_sleeps(void);                // how many times since boot
