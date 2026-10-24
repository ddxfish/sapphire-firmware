// light.h - P1 stub. The Cardputer ADV's RGB LED (and screen cues) come in a
// later phase; for now these are no-ops so net.c / link.c can call them.
#pragma once
#include <stdbool.h>

void light_start(void);
void light_cue(const char *name);     // "idle", "nolink", "listen", "think", "speak"
void light_speaking(bool on);         // TTS playing: show SPEAKING, then back to READY
