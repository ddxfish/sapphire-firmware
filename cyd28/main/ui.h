// ui.h - the screen: one chat, landscape, read a page at a time.
//
//   ┌──────────────────────────────────┐
//   │ ● [kitchen      ▾]  ‹ 3/7 ›  12:34│  the bar: link dot, the chat picker, the page, her state / the clock
//   │ you: what's for dinner            │
//   │ Soup, and the bread from         │  one page of the transcript, drawn once.
//   │ yesterday. I put the ...         │  Swipe or the arrows turn it; a tap on 3/7 goes to the end.
//   │                                  │
//   │ [say something...............][>]│  the input row
//   └──────────────────────────────────┘
//
// While you type, the keyboard takes the page's place: the bar, the input
// row, then the keys. OK sends and puts the page back.
//
// The transcript is the last few entries (yours, hers, a failure), kept as
// text; the whole chat stays with Sapphire. Pages are cut with LVGL's own
// line breaker, so a page is exactly the lines that fit. Nothing scrolls: a
// page is a few labels, a turn of the page replaces them. Her reply arrives
// in pieces (link.c): ui_reply_begin, then ui_reply_set with everything so
// far, then ui_reply_done; it is laid into the last page, a page that fills
// opens the next, and the view follows her unless you paged back.
//
// Every function here may be called from any task: each takes LVGL's lock.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#define REPLY_MAX 3000                 // chars of one reply drawn; longer ends in "..." (the history door cuts there too; link.c pulls that much)

// The pages. A menu at the bar's left edge (where the link dot sits) opens
// them; every page but the chat is built when opened and freed when left
// (pages.c). The board opens on `settings.home`, set from the console or from
// Settings > Devices (PUT /screen/settings {"home"}).
typedef enum { PAGE_CHAT = 0, PAGE_BACKUPS, PAGE_STATUS, PAGE_BOARD, PAGE_ACTIONS, PAGE_COUNT } page_t;
extern const char *PAGE_NAMES[PAGE_COUNT];     // chat, backups, status, board, actions: the words on the wire and the console

void ui_start(void);
bool ui_up(void);                          // the screen is built (the console starts before it)
void ui_copy_text(char *dst, const char *src, size_t size);   // strlcpy that never ends on half a character
void ui_show_page(page_t which);           // from any task
bool ui_set_home(const char *name);        // kept; false = no such page
const char *ui_home_name(void);
void ui_dark(void);                        // the backlight off until a touch
void ui_button_labels(const cJSON *labels);   // {"a": "Lights off"}: the words on the Actions page's buttons
void ui_status(const char *text);          // the bar's right side; "" = the clock
void ui_link(bool connected);              // the dot
void ui_reply_begin(void);                 // a new entry of hers starts
void ui_reply_set(const char *text);       // everything of her reply so far
void ui_reply_done(void);
void ui_failed(const char *why);           // the last thing you typed did not reach her
void ui_mine(const char *text);            // an entry of yours that was not typed: what a button sent
void ui_toast(const char *text, int seconds);   // a line across the top, for a while
void ui_test_card(void);                   // frame, TOP, color bars, a touch echo on the serial line
void ui_calibrate(void);                   // three crosses to tap; CAL lines on the serial line; kept by board.c
void ui_wake(void);                        // the backlight back on
void ui_repaint(void);                     // the panel was turned: everything drawn again

// The chat the board talks in, from Sapphire (POST /screen {"chat"}); its
// picker's list, from link.c (GET /chats). A pick goes out through link.c.
bool ui_chat(const char *name, const char *brain, int trim_rgb);   // true when the name changed: the caller asks for the chat read back
const char *ui_chat_name(void);
const char *ui_brain_name(void);
void ui_chats(const char *joined, const char *current);   // names, one a line

// The chat read back (link.c, GET /history): a batch, newest first. fresh
// = the window starts over (a link, a new chat); else the batch goes before
// what is held and the view stays on the line it was on. `before` is the
// cursor for the next older batch, 0 = nothing older.
void ui_history_begin(bool fresh);
void ui_history_add(bool hers, const char *text);
void ui_history_end(int before);
void ui_history_fail(void);

// A picture takes the whole glass, painted straight onto the panel by whoever
// holds it (server.c). LVGL is held still meanwhile; what she says in that
// time is kept and drawn when the picture goes.
bool ui_picture_begin(void);               // false: the screen is busy
void ui_picture_end(void);
