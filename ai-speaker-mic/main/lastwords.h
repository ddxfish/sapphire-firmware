// lastwords.h - why the board last went down, kept across the reboot.
//
// Three ways down, one record: a watchdog here that finds something stuck
// (lastwords_die: every task's call chain, then a restart), a panic (IDF's
// core dump in its own partition, summed up at the next boot), and the plain
// reset reason for the rest. The record is moved to flash at boot and stays
// until the next one: GET /health carries the short form, the console's
// `lastwords` the call chains. `where` is the same picture of the live board.
// Addresses are decoded on the PC against build/sapphire-ai-speaker-mic.elf
// (xtensa-esp32s3-elf-addr2line -pfiaC -e <elf> <addresses>).
#pragma once
#include <stdbool.h>

#include "cJSON.h"

void lastwords_start(void);                    // at boot, after settings_load (NVS is up)
void lastwords_die(const char *reason) __attribute__((noreturn));   // record every task, restart
const char *lastwords_reset(void);             // this boot's reset reason, in words
int lastwords_strikes(void);                   // watchdog restarts in a row, each within 5 minutes of its boot
cJSON *lastwords_json(bool chains);            // the kept record; NULL when there is none
void lastwords_forget(void);                   // read and understood: the record is dropped
cJSON *lastwords_where(void);                  // every task's call chain right now
