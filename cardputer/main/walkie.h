// walkie.h - the Walkie-Talkie app, driven by the shell (see walkie.c).
#pragma once
#include <stdbool.h>

void walkie_enter(void);                 // becomes the active app: paint its home page
void walkie_exit(void);                  // leaving it: finish any open stream
void walkie_key(int code, bool down);    // a key event while active (Ctrl = push-to-talk)
void walkie_tick(void);                  // called every shell loop while active
bool walkie_recording(void);             // true while holding Ctrl (shell polls faster then)
