// status.c - see status.h.
#include "status.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "link.h"
#include "net.h"
#include "settings.h"

#define STALE_S (36 * 3600)

static const look_t LOOKS[] = {
    [ST_SETUP]    = {255, 255, 255, false, PAT_SOLID, 30, 0.08f, 0.08f, true},
    [ST_WRITING]  = {0, 60, 255, false, PAT_PULSE, 110, 0.05f, 0.9f, true},
    [ST_NOCARD]   = {255, 0, 0, false, PAT_SOLID, 30, 0.25f, 0.25f, true},
    [ST_DAMAGED]  = {255, 0, 0, false, PAT_BLINK, 80, 0.0f, 0.6f, true},
    [ST_CHECKING] = {0, 255, 255, false, PAT_PULSE, 30, 0.05f, 0.5f, true},
    [ST_FAILED]   = {255, 0, 0, false, PAT_SOLID, 30, 0.25f, 0.25f, true},
    [ST_EMPTY]    = {255, 140, 0, false, PAT_SOLID, 30, 0.2f, 0.2f, true},
    [ST_OFFLINE]  = {255, 140, 0, false, PAT_PULSE, 20, 0.05f, 0.3f, true},
    [ST_STALE]    = {255, 140, 0, false, PAT_SOLID, 30, 0.2f, 0.2f, true},
    [ST_GOOD]     = {0, 255, 0, false, PAT_SOLID, 30, 0.15f, 0.15f, true},
};
static const char *WORDS[] = {"setup", "writing", "nocard", "damaged", "checking", "failed", "empty", "offline", "stale", "good"};

// sapphire_2026-10-09_214003_daily.tar.gz.sapphirebak -> its moment, in the board's own time zone
long status_newest_at(const char *name)
{
    struct tm t = {0};
    int h = 0;
    if (sscanf(name, "sapphire_%d-%d-%d_%d_", &t.tm_year, &t.tm_mon, &t.tm_mday, &h) != 4) return 0;
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    t.tm_hour = h / 10000;
    t.tm_min = h / 100 % 100;
    t.tm_sec = h % 100;
    t.tm_isdst = -1;
    return (long)mktime(&t);
}

status_t status_now(storage_state_t *card)
{
    storage_state_t mine;
    if (!card) card = &mine;
    storage_state(card);
    if (!settings_ready()) return ST_SETUP;
    if (card->writing) return ST_WRITING;
    if (!card->mounted) return ST_NOCARD;
    if (card->damaged) return ST_DAMAGED;
    if (card->checking) return ST_CHECKING;
    if (card->failed) return ST_FAILED;
    if (!card->count) return ST_EMPTY;
    if (!net_up() || !link_connected()) return ST_OFFLINE;
    time_t now = time(NULL);
    long made = status_newest_at(card->newest);
    if (now > 1700000000 && made && now - made > STALE_S) return ST_STALE;   // only once the clock is set
    return ST_GOOD;
}

const look_t *status_look(status_t s) { return &LOOKS[s]; }
bool status_loud(status_t s) { return s == ST_WRITING || s == ST_NOCARD || s == ST_DAMAGED || s == ST_CHECKING || s == ST_FAILED; }
const char *status_word(status_t s) { return WORDS[s]; }
