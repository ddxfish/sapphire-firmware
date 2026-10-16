// link.h - what the board says to Sapphire, over HTTPS with its own key.
//
//   POST /api/devices/<name>/text          what was typed; answered once a turn starts
//   POST /api/devices/<name>/press         a button set to send her a message was pressed
//   GET  /api/devices/<name>/events        held open: her state for the light, what the
//                                          buttons are set to, and "text" doorbells:
//                                          {msg, rev, have, done}
//   GET  /api/devices/<name>/text?msg=&rev=&from=&max=
//                                          the words of her reply from `from` on
//   GET  /api/devices/<name>/chats         the chats the board may pick from, and the one it talks in
//   POST /api/devices/<name>/chat          {"name"}: the pick; the name comes back through POST /screen
//   GET  /api/devices/<name>/history?before=&max=   the chat read back, newest first, for the page
//
// Her reply does not ride the events stream: that stream may drop a line when
// it is behind, and a dropped line of prose is garbage on the screen. A
// doorbell says only how much there is; the board pulls what it lacks. A
// missed doorbell is made good by the next one; a new rev of the same message
// (her final text differs from what streamed) is pulled again from the start.
//
// One connection holds the stream; one more comes and goes for a post or a
// pull, through a single client task, so two TLS sessions is the most this
// board ever holds (about 50 KB).
#pragma once
#include <stdbool.h>

void link_start(void);
bool link_send_text(const char *text);     // queued for the client task; false when there is no link
void link_press(const char *button, const char *how);   // a press set to reach her: queued like typed text
void link_ask_chats(void);                 // GET /chats: the picker's list, handed to ui_chats when it comes
void link_pick_chat(const char *name);     // POST /chat {"name"}: the board talks there from now on
void link_ask(const char *job);            // POST /ask {"job"}: work that is hers (backup); her line shows as a toast
void link_ask_history(int before);         // GET /history: the chat read back for the page; before < 0 = the newest, else the cursor
bool link_connected(void);                 // the events stream is open
