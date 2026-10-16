// pages.h - the pages besides the chat, each built when opened and freed when
// left (ui.c owns the menu and the switching). Every builder is called under
// LVGL's lock and returns the page's container, a child of `parent`; a page
// that refreshes itself keeps an lv_timer and deletes it with the container.
//
//   status    what the board knows about itself and its link
//   backups   the card: what is on it, how much room, a backup now, progress as one lands
//   board     the board's own knobs: flashlight, brightness, a tone, screen off, restart
//   actions   nine buttons of your own, each set in Settings > Devices > Buttons; none set by default
#pragma once
#include "lvgl.h"

#define VBUTTONS 9
#define VLABEL_MAX 48                  // bytes: Sapphire cuts a label at 24 characters, which may be more bytes

lv_obj_t *page_status_build(lv_obj_t *parent);
lv_obj_t *page_backups_build(lv_obj_t *parent);
lv_obj_t *page_board_build(lv_obj_t *parent);
lv_obj_t *page_actions_build(lv_obj_t *parent);
void page_actions_label(int which, const char *words);   // a button's words changed (ui.c, from the stream); under the lock
