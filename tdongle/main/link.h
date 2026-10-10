// link.h - what the stick says to Sapphire, over HTTPS with its own key.
//
//   POST /api/devices/<name>/press    a button was pressed: {"button", "how"} and, for a job
//                                     that is Sapphire's to do, "job" ("backup": back up to me now)
//   GET  /api/devices/<name>/events   held open: the clock, and what the buttons are set to
//
// One connection holds the stream; one more comes and goes for a press.
#pragma once
#include <stdbool.h>

void link_start(void);
void link_press(const char *button, const char *how);   // a press set to send her a message (buttons.c)
void link_ask(const char *job);                         // a job that is hers to do: "backup"
bool link_connected(void);                              // the events stream is open
