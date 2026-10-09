// pet.h - the creature's body. It lives on its own page: a round thing that
// breathes, blinks, watches your finger, bounces when poked, and shows what
// it needs. Its LIFE (hunger, mood, growth, her part in it) is Sapphire's
// (parked); this file only draws a state and reports touches.
// Until the life exists the state is a stand-in and the buttons play out
// locally, so the page can be seen and felt.
#pragma once
#include <stdbool.h>

#include "lvgl.h"

typedef enum { PET_IDLE, PET_EATING, PET_PLAYING, PET_SLEEPING, PET_WASHING } pet_doing_t;

typedef struct {
    char name[24];
    int stage;                         // 0 hatchling, 1 kid, 2 grown
    float mood, hunger, energy, clean; // 0..1
    pet_doing_t doing;
    char says[48];
} pet_state_t;

void pet_build(lv_obj_t *page);        // under LVGL's lock
void pet_set_state(const pet_state_t *s);   // from any task
void pet_shown(bool on);               // the page is on / off screen: animations run only while on
