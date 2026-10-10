// storage.h - the TF card as a place Sapphire keeps backups.
//
// The satellite protocol's `storage` (docs/SATELLITE-PROTOCOL.md): four doors
// in server.c, the card itself here. Mounted once at boot, after the board
// (the card's CS line is on the expander); no card = no storage, and /health
// says no `storage` in `has`. The card is plain FAT32 so any computer reads
// it; files live in /sapphire-backups on the card.
//
// Nothing plaintext lands here: a backup's first 12 bytes must be the
// SAPPHIREBAK magic (the four opener files are the only plain names). That
// is the board's own check, on top of Sapphire's.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STORAGE_NAME_MAX 96

bool storage_start(void);                     // mount, sweep .partial files
bool storage_ok(void);                        // a card is mounted
bool storage_room(uint64_t *free_bytes, uint64_t *total_bytes);
const char *storage_dir(void);                // the folder on the card
bool storage_name_ok(const char *name);       // [A-Za-z0-9_.-]{1,96}, no leading dot
bool storage_name_plain_ok(const char *name); // one of the opener files (plain is allowed)
bool storage_path(const char *name, char *out, size_t size);     // dir + name

// Writing: open name.partial, feed it, then finish (rename + sha256) or drop.
typedef struct storage_write storage_write_t;
storage_write_t *storage_write_begin(const char *name, uint64_t expect);
bool storage_write_feed(storage_write_t *w, const uint8_t *data, size_t n);
bool storage_write_finish(storage_write_t *w, char sha_hex[65], uint64_t *size);
void storage_write_drop(storage_write_t *w);

bool storage_format(void);                    // wipe + FAT32; refuses while busy
bool storage_busy(void);                      // a write or a format is on
bool storage_remove(const char *name);        // a file and the hash beside it

// A board whose board.h says BOARD_STORAGE_CHECKS reads every backup back
// after writing it, keeps its sha256 beside it (name.sha256, never listed)
// and checks them all once after the card is mounted. That costs a second
// read of each backup and a 16 KB buffer for the while; a board that cannot
// spare the time or the card traffic leaves the define out and nothing changes.
typedef struct {
    bool mounted;
    int count;                                // sealed backups on the card
    int damaged;                              // of those: no longer what was written (the check found them)
    int unchecked;                            // of those: no hash beside them
    char newest[STORAGE_NAME_MAX + 1];        // "" when there is none
    bool checking;                            // the check after the mount is on
    int checked, to_check;
    bool writing;                             // a backup is arriving: do not unplug
    uint64_t got, expect;
    bool failed;                              // the last backup sent here did not land
} storage_state_t;
void storage_state(storage_state_t *out);
bool storage_check_now(void);                 // every backup read back against its hash, again; false = one is running, or no card (BOARD_STORAGE_CHECKS only)
int storage_checked(const char *name);        // 1 = has a hash and matched, 0 = damaged, -1 = no hash kept
