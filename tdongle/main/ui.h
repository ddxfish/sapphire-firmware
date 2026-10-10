// ui.h - the screen: 160 x 80, two lines of large text that say how the
// backups are (status.h says the same on the LED).
//
//   ┌────────────────┐   ┌────────────────┐   ┌────────────────┐
//   │  12/29 GB      │   │  WRITE 43%     │   │  2 DAMAGED     │
//   │  14  Oct 9     │   │  do not pull   │   │  12/29 GB      │
//   └────────────────┘   └────────────────┘   └────────────────┘
//    room used, how many   a backup arriving     the check at start
//    backups, the newest                         found bad ones
//
// The backlight goes dark after two minutes; the button, a change of status
// or a line from Sapphire brings it back. Every function may be called from
// any task.
#pragma once
#include <stdbool.h>

void ui_start(void);
void ui_toast(const char *text, int seconds);   // a line of hers (or the board's) over the two lines, for a while
bool ui_wake(void);                             // the backlight back on; true when it was dark
void ui_sleep(void);                            // dark now
void ui_test_card(void);                        // red, green, blue and which way is up, for 20 s
