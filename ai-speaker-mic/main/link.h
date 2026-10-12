// link.h - the board's side of the three doors on Sapphire:
//   POST /api/devices/<name>/wake     at the wake word: may this board take it?
//   POST /api/devices/<name>/voice    what it heard, after its wake word
//   GET  /api/devices/<name>/events   held open: what its light should show, and what its buttons are set to
//   POST /api/devices/<name>/press    a button set to send her a message was pressed
// Sapphire is HTTPS with her own certificate. When the settings hold it, it
// is the only one trusted. When they do not, the connection is not checked.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void link_start(void);                 // once WiFi is up: holds the events stream open
bool link_connected(void);
// Sends a 16 kHz mono recording. True when Sapphire took it. `heard` gets
// what she made of it, for the log.
bool link_send_voice(const int16_t *pcm, size_t samples, char *heard, size_t heard_size);
// Asks for the wake that just fired. False only when Sapphire said another
// listener has it (`taken_by` names it); when she cannot be asked, true.
bool link_claim_wake(char *taken_by, size_t size);
// A press Sapphire set to reach her ("boot", "short"): queued, sent by its own task. Never blocks.
void link_press(const char *button, const char *how);
void link_ask(const char *job);         // work that is Sapphire's, from any task: "stop" (she was stopped here), "backup" (onto this board's card, now)
