// buttons.h - the board's real buttons (docs/SATELLITE-PROTOCOL.md, "A button
// was pressed"). The same file on every board; a board hands it its buttons
// and the jobs it can do by itself.
//
// A button is pressed three ways: short, long (held LONG_MS, fires while
// held) and double. Each press does one job:
//   - what Sapphire set for it (`bound`, from the events stream, kept in
//     memory only): "tell" sends the press to her (link_press), a job's name
//     runs that job here, any other word is nothing
//   - else the board's own job for it, when it has one
// A short press waits DOUBLE_MS for a second one only on a button that has
// a job for its double press.
//
// The pins are read every 5 ms by a timer of their own; buttons_poll only
// acts on what it saw. So a slow or late poll delays a job, never loses a press.
//
// A button that is not on a pin of the chip (gpio below 0):
//   BUTTON_FED  the board reads it its own way (a key on a port expander) and
//               says how it sits with buttons_feed, every 20 to 60 ms. Pressed
//               the same three ways; a press shorter than two feeds can be missed.
//   BUTTON_TAP  a place on a touch screen: buttons_tap is one short press of
//               it, its only way. /health says so (`ways`).
//
// BOOT (GPIO 0) is an ordinary button while the program runs: the chip reads
// it only as it starts. So no job may restart the board: held through a
// restart, it is the chip's download mode.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

enum { PRESS_SHORT, PRESS_LONG, PRESS_DOUBLE, PRESS_WAYS };

typedef struct { const char *name, *words; void (*run)(void); } button_job_t;   // words: what the Devices page calls it
#define BUTTON_FED (-1)
#define BUTTON_TAP (-2)
#define BUTTON_JOB 16                       // a job's name, with its end
typedef struct { const char *name; int gpio; const char *own[PRESS_WAYS]; } button_def_t;   // low = pressed; own: its jobs when left to the board, or NULL

// Both tables must stay where they are (static). Sets each pin as an input with a pull-up.
void buttons_setup(const button_def_t *list, int count, const button_job_t *jobs, int job_count);
void buttons_feed(int button, bool down);   // a BUTTON_FED button as it sits now; always from the same task
void buttons_tap(const char *name);         // a BUTTON_TAP button was tapped; from any task, the job runs in buttons_poll
bool buttons_does(const char *name, int way, char *job, size_t size);   // the job that press does now ("" and false = none); from any task
void buttons_poll(void);                    // every 20 to 60 ms, always from the same task; jobs run on it
void buttons_flush(void);                   // presses so far are forgotten (the one that ended a picture); from any task
void buttons_bound(const cJSON *bound);     // {"boot": {"short": "tell", "double": "listen"}}, from any task
cJSON *buttons_json(void);                  // for /health: {"list": [...], "can": {...}}
