// ui.c - see ui.h. LVGL 9 through esp_lvgl_port, which runs LVGL on its own
// task and hands out the lock.
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "board.h"
#include "link.h"
#include "pet.h"
#include "settings.h"

static const char *TAG = "ui";
#define BUBBLES_MAX 12                 // the window: older bubbles go, the text is hers to keep
#define BUBBLE_W (BOARD_W - 50)
#define REPLY_MAX 2400                 // chars of one reply drawn; longer ends in "..."
#define KEYBOARD_H 170
#define TABBAR_H 40
#define DIM_AFTER_S 90                 // untouched this long: backlight to a glow
#define DIM_PERCENT 8

static lv_display_t *disp;
static lv_indev_t *indev;
static lv_obj_t *bar, *dot, *name_lbl, *status_lbl, *list, *input, *send_btn, *keyboard, *toast;
static lv_obj_t *pages[2], *tabs[2];      // 0 the chat, 1 the pet
static int page;
static void show_page(int which);
static lv_obj_t *reply;                // her bubble being written, or NULL
static lv_timer_t *toast_timer, *clock_timer, *watch_timer;
static char status_text[48];
static bool dimmed;
static int64_t button_down_ms;
static volatile bool picture_on;           // server.c holds the glass and the LVGL lock
static bool pending_begin;                 // a reply began behind the picture
static char *pending_reply;                // its text so far

#define C_BG lv_color_hex(0x0f1216)
#define C_BAR lv_color_hex(0x171c23)
#define C_HERS lv_color_hex(0x1f3d66)
#define C_MINE lv_color_hex(0x2b3038)
#define C_TEXT lv_color_hex(0xe8ecf1)
#define C_DIM lv_color_hex(0x8a93a0)
#define C_OK lv_palette_main(LV_PALETTE_GREEN)
#define C_BAD lv_palette_main(LV_PALETTE_RED)

// --- the chat font ----------------------------------------------------------------------
// 14, 16, 18 or 20 px, chosen with the console's `screen font N` and kept in flash.

static const lv_font_t *chat_font(void)
{
    switch (settings.font) {
    case 14: return &lv_font_montserrat_14;
    case 16: return &lv_font_montserrat_16;
    case 20: return &lv_font_montserrat_20;
    default: return &lv_font_montserrat_18;
    }
}

// --- her words on a small font -------------------------------------------------------
// Montserrat here has ASCII, the degree sign and the bullet. The curly and the
// long dashes she writes become their plain cousins, and the markdown that
// means nothing on a screen this size is dropped.

static void fold(char *s)
{
    char *w = s;
    for (const unsigned char *r = (const unsigned char *)s; *r;) {
        if (r[0] == 0xE2 && r[1] == 0x80 && r[2]) {
            unsigned char c = r[2];
            const char *put = c == 0x94 || c == 0x93 ? "-" : c == 0x99 || c == 0x98 ? "'"
                            : c == 0x9C || c == 0x9D ? "\"" : c == 0xA6 ? "..." : c == 0xA2 ? "\xE2\x80\xA2" : NULL;
            if (put) { size_t n = strlen(put); memcpy(w, put, n); w += n; r += 3; continue; }
        }
        if (r[0] == 0xC2 && r[1] == 0xA0) { *w++ = ' '; r += 2; continue; }
        if (r[0] == '*' && r[1] == '*') { r += 2; continue; }
        if (r[0] == '`') { r++; continue; }
        if (r[0] == '#' && (w == s || w[-1] == '\n')) { while (*r == '#') r++; while (*r == ' ') r++; continue; }
        *w++ = *r++;
    }
    *w = 0;
}

// --- bubbles ------------------------------------------------------------------------

static void scroll_to_end(void)
{
    lv_obj_update_layout(list);
    uint32_t n = lv_obj_get_child_count(list);
    if (n) lv_obj_scroll_to_view(lv_obj_get_child(list, n - 1), LV_ANIM_OFF);
}

static lv_obj_t *bubble(bool hers, const char *text)
{
    while (lv_obj_get_child_count(list) >= BUBBLES_MAX) lv_obj_delete(lv_obj_get_child(list, 0));
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, hers ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(l, BUBBLE_W, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(l, hers ? C_HERS : C_MINE, 0);
    lv_obj_set_style_radius(l, 9, 0);
    lv_obj_set_style_pad_hor(l, 8, 0);
    lv_obj_set_style_pad_ver(l, 5, 0);
    lv_obj_set_style_text_color(l, C_TEXT, 0);
    lv_obj_set_style_text_font(l, chat_font(), 0);
    lv_label_set_text(l, text);
    scroll_to_end();
    return l;
}

// --- the input row and the keyboard ----------------------------------------------------

static void keyboard_show(bool on)
{
    if (on) lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    scroll_to_end();
}

static void send_now(void)
{
    const char *text = lv_textarea_get_text(input);
    while (*text == ' ') text++;
    if (!*text) return;
    bubble(false, text);
    if (!link_send_text(text)) ui_failed("not sent: no link");
    lv_textarea_set_text(input, "");
}

static void on_input(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) keyboard_show(true);
    else if (code == LV_EVENT_READY) { send_now(); keyboard_show(false); lv_obj_remove_state(input, LV_STATE_FOCUSED); }
    else if (code == LV_EVENT_CANCEL) { keyboard_show(false); lv_obj_remove_state(input, LV_STATE_FOCUSED); }
}

static void on_send(lv_event_t *e) { send_now(); keyboard_show(false); lv_obj_remove_state(input, LV_STATE_FOCUSED); }

static void on_list_tap(lv_event_t *e)
{
    if (!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN)) { keyboard_show(false); lv_obj_remove_state(input, LV_STATE_FOCUSED); }
}

// --- the bar, the clock, the backlight, the button ---------------------------------------

static void draw_status(void)
{
    if (status_text[0]) { lv_label_set_text(status_lbl, status_text); return; }
    time_t now = time(NULL);
    if (now < 1600000000) { lv_label_set_text(status_lbl, ""); return; }
    struct tm t;
    localtime_r(&now, &t);
    char text[8];
    strftime(text, sizeof text, "%H:%M", &t);
    lv_label_set_text(status_lbl, text);
}

static void on_clock(lv_timer_t *t) { draw_status(); }

static void on_watch(lv_timer_t *t)
{
    // the backlight: a glow after a while untouched, back at a touch or the button
    uint32_t idle = lv_display_get_inactive_time(disp);
    if (!dimmed && idle > DIM_AFTER_S * 1000) { dimmed = true; board_backlight(DIM_PERCENT); }
    else if (dimmed && idle < 1000) { dimmed = false; board_backlight(settings.brightness); }
    // BOOT: a tap brings the keyboard; two seconds clears the window
    int64_t now = lv_tick_get();
    if (board_button()) {
        if (!button_down_ms) button_down_ms = now;
        else if (now - button_down_ms > 2000) {
            lv_obj_clean(list);
            reply = NULL;
            button_down_ms = -1;                       // done until released
        }
    } else if (button_down_ms > 0) {
        if (dimmed) { dimmed = false; board_backlight(settings.brightness); }
        else if (page != 0) show_page(0);
        else keyboard_show(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        button_down_ms = 0;
    } else {
        button_down_ms = 0;
    }
}

static void on_toast_end(lv_timer_t *t)
{
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(toast_timer);
    toast_timer = NULL;
}

// --- the pages --------------------------------------------------------------------------

static void show_page(int which)
{
    page = which;
    for (int i = 0; i < 2; i++) {
        if (i == which) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(tabs[i], i == which ? C_TEXT : C_DIM, 0);
    }
    if (which != 0) { lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_state(input, LV_STATE_FOCUSED); }
    else scroll_to_end();
    pet_shown(which == 1);
}

static void on_tab(lv_event_t *e) { show_page((int)(intptr_t)lv_event_get_user_data(e)); }

// --- the screen ----------------------------------------------------------------------

static void build(void)
{
    lv_theme_t *theme = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_CYAN),
                                              true, chat_font());
    lv_display_set_theme(disp, theme);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // the bar
    bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 26);
    lv_obj_set_style_bg_color(bar, C_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    dot = lv_obj_create(bar);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, C_BAD, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
    name_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name_lbl, C_DIM, 0);
    lv_label_set_text(name_lbl, settings.name);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 14, 0);
    status_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(status_lbl, C_DIM, 0);
    lv_label_set_text(status_lbl, "");
    lv_obj_align(status_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    // two pages: the chat, the pet. One shows.
    for (int i = 0; i < 2; i++) {
        pages[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(pages[i]);
        lv_obj_set_width(pages[i], LV_PCT(100));
        lv_obj_set_flex_grow(pages[i], 1);
        lv_obj_set_flex_flow(pages[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(pages[i], 0, 0);
        lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_add_flag(pages[1], LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *chat = pages[0];

    // the chat window
    list = lv_obj_create(chat);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_set_style_pad_row(list, 5, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(list, on_list_tap, LV_EVENT_CLICKED, NULL);

    // the input row
    lv_obj_t *row = lv_obj_create(chat);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_style_bg_color(row, C_BAR, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    input = lv_textarea_create(row);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_placeholder_text(input, "say something");
    lv_textarea_set_max_length(input, 500);
    lv_obj_set_flex_grow(input, 1);
    lv_obj_set_height(input, 38);
    lv_obj_set_style_pad_ver(input, 8, 0);
    lv_obj_add_event_cb(input, on_input, LV_EVENT_ALL, NULL);
    send_btn = lv_button_create(row);
    lv_obj_set_size(send_btn, 46, 38);
    lv_obj_t *arrow = lv_label_create(send_btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_center(arrow);
    lv_obj_add_event_cb(send_btn, on_send, LV_EVENT_CLICKED, NULL);

    // the keyboard
    keyboard = lv_keyboard_create(chat);
    lv_obj_set_size(keyboard, LV_PCT(100), KEYBOARD_H);
    lv_keyboard_set_textarea(keyboard, input);
    lv_keyboard_set_popovers(keyboard, false);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_16, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    // the pet, on its page
    pet_build(pages[1]);

    // the tab bar
    lv_obj_t *tabbar = lv_obj_create(scr);
    lv_obj_remove_style_all(tabbar);
    lv_obj_set_size(tabbar, LV_PCT(100), TABBAR_H);
    lv_obj_set_flex_flow(tabbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabbar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(tabbar, C_BAR, 0);
    lv_obj_set_style_bg_opa(tabbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(tabbar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(tabbar, lv_color_hex(0x2a3038), 0);
    lv_obj_set_style_border_width(tabbar, 1, 0);
    static const char *names[2] = {LV_SYMBOL_KEYBOARD "  Chat", LV_SYMBOL_EYE_OPEN "  Pet"};
    for (int i = 0; i < 2; i++) {
        tabs[i] = lv_label_create(tabbar);
        lv_label_set_text(tabs[i], names[i]);
        lv_obj_set_style_text_font(tabs[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_hor(tabs[i], 18, 0);
        lv_obj_set_style_pad_ver(tabs[i], 10, 0);
        lv_obj_add_flag(tabs[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tabs[i], on_tab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    show_page(0);

    // a line across the top for what she shows
    toast = lv_label_create(lv_layer_top());
    lv_obj_set_width(toast, LV_PCT(100));
    lv_label_set_long_mode(toast, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_color(toast, lv_palette_darken(LV_PALETTE_BLUE, 3), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(toast, 8, 0);
    lv_obj_set_style_text_color(toast, C_TEXT, 0);
    lv_obj_set_style_text_align(toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

    clock_timer = lv_timer_create(on_clock, 10000, NULL);
    watch_timer = lv_timer_create(on_watch, 60, NULL);
}

void ui_start(void)
{
    const lvgl_port_cfg_t port = {.task_priority = 4, .task_stack = 8192, .task_affinity = 1, .task_max_sleep_ms = 500, .timer_period_ms = 5};
    ESP_ERROR_CHECK(lvgl_port_init(&port));
    board_t *b = board();
    const lvgl_port_display_cfg_t dcfg = {
        .io_handle = b->io, .panel_handle = b->panel,
        .buffer_size = BOARD_W * BOARD_DRAW_LINES, .double_buffer = true,
        .hres = BOARD_W, .vres = BOARD_H, .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = false, .mirror_x = !b->flip, .mirror_y = b->flip},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    disp = lvgl_port_add_disp(&dcfg);
    if (b->touch) {
        const lvgl_port_touch_cfg_t tcfg = {.disp = disp, .handle = b->touch};
        indev = lvgl_port_add_touch(&tcfg);
        if (!indev) ESP_LOGE(TAG, "LVGL did not take the touch chip");
    }
    lvgl_port_lock(0);
    build();
    lvgl_port_unlock();
    board_backlight(settings.brightness);
    ESP_LOGI(TAG, "screen up, %s", b->touch ? "touch on" : "NO touch");
}

// --- from other tasks ------------------------------------------------------------------

void ui_status(const char *text)
{
    if (!disp) return;
    strlcpy(status_text, text ? text : "", sizeof status_text);
    if (picture_on) return;                // drawn when the picture goes
    lvgl_port_lock(0);
    draw_status();
    lvgl_port_unlock();
}

void ui_link(bool connected)
{
    if (!disp || picture_on) return;
    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(dot, connected ? C_OK : C_BAD, 0);
    lvgl_port_unlock();
}

void ui_reply_begin(void)
{
    if (!disp) return;
    if (picture_on) { pending_begin = true; free(pending_reply); pending_reply = NULL; return; }
    lvgl_port_lock(0);
    reply = bubble(true, "...");
    lvgl_port_unlock();
}

void ui_reply_set(const char *text)
{
    if (!disp) return;
    if (picture_on) { free(pending_reply); pending_reply = strdup(text ? text : ""); return; }
    lvgl_port_lock(0);
    if (!reply) reply = bubble(true, "");
    char *copy = strndup(text ? text : "", REPLY_MAX + 4);
    if (copy) {
        if (strlen(copy) > REPLY_MAX) strcpy(copy + REPLY_MAX, "...");
        fold(copy);
        lv_label_set_text(reply, *copy ? copy : "...");
        free(copy);
    }
    scroll_to_end();
    lvgl_port_unlock();
}

void ui_reply_done(void)
{
    // the bubble stays the current one: a last piece that lands late still goes
    // in it, not in a new bubble (two bubbles for one reply, 2026-10-06)
    if (!disp || picture_on) return;
    lvgl_port_lock(0);
    if (reply && !strcmp(lv_label_get_text(reply), "...")) lv_label_set_text(reply, "(no answer)");
    lvgl_port_unlock();
}

void ui_failed(const char *why)
{
    if (!disp || picture_on) return;
    lvgl_port_lock(0);
    lv_obj_t *l = bubble(false, why);
    lv_obj_set_style_text_color(l, C_BAD, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lvgl_port_unlock();
}

void ui_toast(const char *text, int seconds)
{
    if (!disp || picture_on) return;
    lvgl_port_lock(0);
    char *copy = strndup(text ? text : "", 300);
    if (copy) { fold(copy); lv_label_set_text(toast, copy); free(copy); }
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
    if (toast_timer) lv_timer_delete(toast_timer);
    toast_timer = lv_timer_create(on_toast_end, (seconds > 0 ? seconds : 8) * 1000, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);
    ui_wake();
    lvgl_port_unlock();
}

void ui_wake(void)
{
    if (dimmed) { dimmed = false; board_backlight(settings.brightness); }
    if (disp) lv_display_trigger_activity(disp);
}

bool ui_picture_begin(void)
{
    if (!disp || picture_on || !lvgl_port_lock(1500)) return false;
    picture_on = true;
    if (dimmed) { dimmed = false; board_backlight(settings.brightness); }
    return true;
}

void ui_picture_end(void)
{
    if (!picture_on) return;
    picture_on = false;
    lv_display_trigger_activity(disp);
    lv_obj_invalidate(lv_screen_active());  // everything is repainted over the picture
    lv_obj_invalidate(lv_layer_top());
    draw_status();
    lvgl_port_unlock();
    if (pending_begin) { pending_begin = false; ui_reply_begin(); }
    if (pending_reply) { char *t = pending_reply; pending_reply = NULL; ui_reply_set(t); free(t); }
}

// --- the test card ------------------------------------------------------------------------

static void on_card_touch(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    printf("TOUCH %d %d\n", (int)p.x, (int)p.y);
    fflush(stdout);
}

static void on_card_back(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_user_data(e);
    lv_obj_delete(card);
}

void ui_test_card(void)
{
    if (!disp) return;
    lvgl_port_lock(0);
    lv_obj_t *card = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, on_card_touch, LV_EVENT_PRESSED, NULL);
    static const struct { const char *name; uint32_t hex; } bars[] = {{"RED", 0xff0000}, {"GREEN", 0x00ff00}, {"BLUE", 0x0000ff}, {"WHITE", 0xffffff}};
    int step = (BOARD_W - 16) / 4, wide = step - 8, tall = BOARD_H / 3;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *bar_ = lv_obj_create(card);
        lv_obj_remove_style_all(bar_);
        lv_obj_set_size(bar_, wide, tall);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(bars[i].hex), 0);
        lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, 0);
        lv_obj_set_pos(bar_, 8 + i * step, BOARD_H / 4);
        lv_obj_remove_flag(bar_, LV_OBJ_FLAG_CLICKABLE);          // a tap on a bar is a tap on the card
        lv_obj_t *l = lv_label_create(card);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_label_set_text(l, bars[i].name);
        lv_obj_set_pos(l, 8 + i * step, BOARD_H / 4 + tall + 6);
    }
    lv_obj_t *top = lv_label_create(card);
    lv_obj_set_style_text_font(top, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(top, lv_color_white(), 0);
    lv_label_set_text(top, "TOP");
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_label_set_text(hint, "tap anywhere: the serial line\nsays where. USB plug below.");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_size(btn, 140, 48);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_event_cb(btn, on_card_back, LV_EVENT_CLICKED, card);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "done");
    lv_obj_center(bl);
    ui_wake();
    lvgl_port_unlock();
}
