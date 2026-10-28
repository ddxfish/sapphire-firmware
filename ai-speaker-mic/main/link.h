// link.h - the board's side of the three doors on Sapphire:
//   POST /api/devices/<name>/wake     at the wake word: may this board take it?
//   POST /api/devices/<name>/voice    what it heard, after its wake word
//   POST /api/devices/<name>/room     a piece of the room, while a capture goes on (capture.c)
//   GET  /api/devices/<name>/events   held open: what its light should show, and what its buttons are set to
//   POST /api/devices/<name>/press    a button set to send her a message was pressed
//   GET  /api/devices/<name>/chats    the chats this board may talk in, for the picker on its glass (0.9.1)
//   POST /api/devices/<name>/chat     {"name"}: the pick; kept as the board's "Talks in chat", the name comes back through POST /screen
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
// A piece of the room: `chunk` is its number within capture `capture_id`, `fires` the JSON list of
// [sample offset, score x 1000] of the wake fires in it (`n_fires` counts them all, listed or not).
// Answers the HTTP status, 0 when Sapphire could not be reached, 204 when she took it but nobody
// on her side is collecting (the capture should end).
int link_send_room(const int16_t *pcm, size_t samples, const char *capture_id, int chunk, const char *fires,
                   int n_fires, bool test, const char *model);
// Asks for the wake that just fired. False only when Sapphire said another
// listener has it (`taken_by` names it); when she cannot be asked, true.
bool link_claim_wake(char *taken_by, size_t size);
// A press Sapphire set to reach her ("boot", "short"): queued, sent by its own task. Never blocks.
void link_press(const char *button, const char *how);
// A tap on a stream that asked for the taps (ui.c): the place by name, raw, with no cue on the ring
// (a game answers on the glass, not with a turn). Queued like a press.
void link_tap(const char *zone);
void link_ask(const char *job);         // work that is Sapphire's, from any task: "stop" (she was stopped here), "backup" (onto this board's card, now)
void link_ask_chats(void);              // GET /chats: her list opens the picker on the glass when it comes (ui_chats_open)
void link_pick_chat(const char *name);  // POST /chat {"name"}: the board talks there from now on; Sapphire sends the name back
