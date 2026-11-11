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
//
// The dead battery (0.5.5). A tired cell carries the chip until the rail or the radio's first
// burst, sags under the brownout level, the chip resets itself, the unloaded cell recovers in
// seconds, the board boots again: a loop that ticks the speaker every lap and over-discharges a
// cell with no protection board. The board has no battery sense, so the brownouts are the gauge:
// the second in a row, within two minutes of the boot before it, and this boot does not try: the
// rail and the amplifier go off and the chip deep-sleeps on a timer alone (no GPIO 0, so no strap
// trap), one minute, then two, four, up to thirty, and tries once at each wake with the sound off
// for two minutes; two minutes without a brownout clears the count. RESET or a power cycle is
// "try now". A shutdown (power_off) is deep sleep with no wake at all: RESET or a replug.
#pragma once
#include <stdbool.h>

#include "cJSON.h"

void power_start(void);                // at boot, after lastwords and before board_init: reads the sleep trail the last life left in RTC memory (it survives the RESET button); a dead-battery boot deep-sleeps here and does not return
bool power_doubt(void);                // this boot is the dead-battery guard's try: the amplifier stays off until two minutes pass without a brownout (power.c turns it on)
void power_off(void) __attribute__((noreturn));   // /power shutdown: everything off, deep sleep with no wake source; RESET or a power cycle brings it back
cJSON *power_json(void);               // /health "sleep": sleeps this boot, awake_s, no_sleep, the trail (last) and the one found at boot (before_reset)
void power_sleep_soon(void);           // from any task: main's watch task does it within a second
void power_restart_soon(void);         // the same for a restart (settings are flushed first, from an internal stack)
bool power_asleep(void);
bool power_no_sleep(void);             // the last reset was a brownout: sleep is refused until a power cycle
bool power_sleep_pending(void);
void power_tick(void);                 // every second from the watch task (internal stack): the timer, the asks
int power_awake_s(void);               // since boot or the last wake
int power_sleeps(void);                // how many times since boot
