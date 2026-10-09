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
