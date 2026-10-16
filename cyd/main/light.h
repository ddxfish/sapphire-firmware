// light.h - the one RGB LED, as Sapphire's "light" and as the board's own face.
// The same words and the same order as the satellite ring (firmware/satellite-
// esp32/main/light.h), painted on a single pixel: spin becomes a quick pulse
// with the hue drifting, rainbow cycles the hue.
//
// A LOOK is a color (or rainbow), a pattern, a speed and two brightnesses:
//   patterns  solid, pulse, blink, heartbeat, spin, rainbow
//   bpm       beats a minute        floor, ceiling  brightness 0..1
//
// What shows, first wins:
//   1. a state of the board: thinking, tool, speaking, error, nolink
//   2. what she set with /led (held for a time, or "off" until cleared)
//   3. the resting look, or outside the active hours the night look (off = dark)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

typedef enum { PAT_SOLID, PAT_PULSE, PAT_BLINK, PAT_HEARTBEAT, PAT_SPIN, PAT_RAINBOW, PAT_COUNT } pattern_t;

typedef struct {
    uint8_t r, g, b;
    bool rainbow;
    pattern_t pattern;
    int bpm;                           // 10..120
    float floor, ceiling;              // 0..1
    bool set;                          // a look at all (false = nothing / dark)
} look_t;

typedef enum { LOOK_RESTING, LOOK_LISTENING, LOOK_THINKING, LOOK_TOOL, LOOK_SPEAKING, LOOK_NOLINK, LOOK_NIGHT, LOOK_COUNT } look_slot_t;

extern const look_t LIGHT_PLAIN;
extern const char *LIGHT_SLOTS[LOOK_COUNT];

void light_start(void);
void light_cue(const char *state);                 // thinking, tool, speaking, error, idle, nolink, starting
void light_cue_from_brain(const char *state);      // the events stream: thinking, tool, idle, error
void light_show(int r, int g, int b, int seconds); // one plain color, for a check
void light_torch(bool on);                         // the LED as a flashlight over everything, until off: warm white, or the color last given
void light_torch_color(uint8_t r, uint8_t g, uint8_t b);   // that color, and on: the Board page's sliders
const uint8_t *light_torch_rgb(void);              // r, g, b
bool light_torch_on(void);

bool light_parse(const cJSON *json, look_t *out, const look_t *base);
void light_hold(const look_t *look, float seconds);
void light_dark(void);
void light_release(void);
void light_set_look(look_slot_t slot, const look_t *look);
const look_t *light_look(look_slot_t slot);
void light_set_hours(int from_min, int until_min);
bool light_in_hours(void);
cJSON *light_json(const look_t *look);
cJSON *light_status(void);
cJSON *light_spec(void);
bool light_save(void);
void light_load(void);
