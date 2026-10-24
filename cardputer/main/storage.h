// storage.h - the microSD card as the OS's disk (IR codes, app data, backups).
//
// Mounted once at boot at /sdcard (FAT32) over its own SPI bus (board.h
// BOARD_SD_*). No card just means the disk is off: apps that need it ask
// storage_ok() first, and the rest of the OS runs fine. General app data is
// plain files under /sdcard/<app>/... with stdio; the sealed-backup protocol
// (like the T-Dongle's) comes with the Backup app, not here.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define STORAGE_MOUNT "/sdcard"

bool storage_start(void);                                 // mount the card (idempotent); false if none / not FAT
void storage_stop(void);                                  // unmount + free the SPI bus (frees ~37 KB for the walkie)
bool storage_ok(void);                                    // a card is mounted
bool storage_room(uint64_t *free_bytes, uint64_t *total_bytes);   // false with no card
const char *storage_card_name(void);                      // the card's CID name, or ""
