// status.h - what the stick is doing, in one word. The LED and the screen
// both show this and nothing else by default: the stick is a backup, and its
// face says whether the backups are safe.
//
//   SETUP      not set up yet                      white, dim
//   WRITING    a backup is arriving: DO NOT UNPLUG blue, quick pulse
//   NOCARD     no card, or not FAT32               red
//   DAMAGED    a backup no longer matches its hash red, blinking
//   CHECKING   reading every backup back at start  cyan, slow pulse
//   FAILED     the last backup did not land        red
//   EMPTY      a card, no backups yet              amber
//   OFFLINE    no WiFi, or Sapphire does not answer amber
//   STALE      the newest backup is over 36 h old  amber
//   GOOD       all of the above is fine            green, dim
#pragma once
#include "light.h"
#include "storage.h"

typedef enum { ST_SETUP, ST_WRITING, ST_NOCARD, ST_DAMAGED, ST_CHECKING, ST_FAILED, ST_EMPTY, ST_OFFLINE, ST_STALE, ST_GOOD } status_t;

status_t status_now(storage_state_t *card);    // card may be NULL
const look_t *status_look(status_t s);
bool status_loud(status_t s);                  // shown over what Sapphire set, and at night
const char *status_word(status_t s);           // for /health: "good", "writing", ...
long status_newest_at(const char *name);       // when a backup was made, from its name; 0 = cannot tell
