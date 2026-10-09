// light.h - the ring of 7, as Sapphire's "light" and as the board's own face.
//
// A LOOK is a color (or rainbow), a pattern, a speed and two brightnesses:
//   patterns  solid, pulse, blink, heartbeat, spin, rainbow
//   color     r, g, b, or rainbow = hues going round
//   bpm       beats (or turns) a minute        floor, ceiling  brightness 0..1
//
// What shows, first wins:
//   1. a state of the board: listening, thinking, tool, speaking, error, nolink
//   2. what she set with /led (held for a time, or "off" until cleared)
//   3. the resting look, or outside the active hours the night look (off = dark)
// The looks for each state, the night look and the hours come from Settings >
// Devices through PUT /led/looks, and are kept in flash. The Pi's ring speaks
// the same words and keeps the same order. Her reply arrives one sentence at
// a time; after each the ring goes back to the brain's last cue (thinking,
// tool) until the brain says idle.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

typedef enum { PAT_SOLID, PAT_PULSE, PAT_BLINK, PAT_HEARTBEAT, PAT_SPIN, PAT_RAINBOW, PAT_COUNT } pattern_t;

typedef struct {
    uint8_t r, g, b;
    bool rainbow;                      // hues go round instead of one color
    pattern_t pattern;
    int bpm;                           // 10..120
    float floor, ceiling;              // 0..1
    bool set;                          // a look at all (false = nothing / dark)
} look_t;

typedef enum { LOOK_RESTING, LOOK_LISTENING, LOOK_THINKING, LOOK_TOOL, LOOK_SPEAKING, LOOK_NOLINK, LOOK_NIGHT, LOOK_COUNT } look_slot_t;

extern const look_t LIGHT_PLAIN;                   // what a "set" starts from

void light_start(void);
void light_cue(const char *state);                 // listening, thinking, tool, speaking, error, camera, idle, nolink, off
void light_cue_from_brain(const char *state);      // the events stream: remembered, painted unless sound plays
void light_speech_done(void);                      // after /audio/speak: the brain's cue if its turn goes on, else idle
void light_show(int r, int g, int b, int seconds); // one plain color on every light, for a check

// --- what /led says ----------------------------------------------------------------
bool light_parse(const cJSON *json, look_t *out, const look_t *base);   // false = a word it does not know
void light_hold(const look_t *look, float seconds);                     // set: shown for a time
void light_dark(void);                                                  // off: dark until clear
void light_release(void);                                               // clear: back to resting
void light_set_look(look_slot_t slot, const look_t *look);
const look_t *light_look(look_slot_t slot);
void light_set_hours(int from_min, int until_min);                      // minutes of the day; same = always
extern const char *LIGHT_SLOTS[LOOK_COUNT];                             // "resting", ... "night": the names /led/looks uses
bool light_in_hours(void);
cJSON *light_json(const look_t *look);                                  // {"color", "animation", "bpm", "floor", "ceiling"}
cJSON *light_status(void);                                              // for /health: {"state", "animation", "blackout"}
const char *light_cue_now(void);                                        // the cue painted now: idle, listening, thinking, ...
void light_current(uint8_t *r, uint8_t *g, uint8_t *b);                 // the ring's color this frame, averaged: the glass mirrors it
bool light_is_dark(void);                                               // off until cleared
void light_toggle_dark(void);                                           // the glass's ring corner
cJSON *light_spec(void);                                                // for /led/spec
bool light_save(void);                                                  // looks and hours to flash
void light_load(void);
