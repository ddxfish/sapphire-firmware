// logring.h - the last lines the board logged, readable over GET /log.
//
// What happened while nobody watched the console: a wake from sleep that "felt
// unstable" (Krem, 2026-11-03) left nothing to read but a reset reason. The log
// hook keeps the newest LOGRING_LINES lines in PSRAM; /log hands them over, oldest
// first. The console still gets every line.
//
// And the boot before this one (0.5.0): PSRAM keeps its contents through a reset of
// the chip (a panic, the watchdog, a brownout, the EN button), and the first block the
// heap hands out lands at the same address every boot of the same program, so the
// ring is found again by its magic. GET /log?last=1 reads it: what the board said on
// its way down, when its death wrote no record at all (2026-11-05).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define LOGRING_LINES 160
#define LOGRING_WIDTH 160

void logring_start(void);                       // first thing in app_main, before the first log line
size_t logring_copy(char *out, size_t cap);     // the lines, oldest first, one per line, NUL-terminated; returns the length
size_t logring_copy_last(char *out, size_t cap);   // the same for the boot before this one; empty when none was found
bool logring_has_last(void);
