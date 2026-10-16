// ui.c - see ui.h. LVGL 9 through esp_lvgl_port, which runs LVGL on its own
// task and hands out the lock. The touch chip is read by hand (board.c) and
// handed to LVGL as an input device of its own.
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lvgl_private.h"              // lv_text_get_next_line: the label's own line breaker, used to cut pages

#include "board.h"
#include "buttons.h"
#include "light.h"
#include "link.h"
#include "pages.h"
#include "settings.h"

static const char *TAG = "ui";
#define ENTRIES_MAX 24                 // the window: what falls out is read back when paged to
#define TEXT_MAX 9000                  // chars of transcript kept, all entries together
#define PAGES_MAX 64
#define BAR_H 28
#define INPUT_H 44
#define PAD 6                          // around the page
#define GAP 4                          // between entries on a page
#define LINE_SPACE 2
#define DIM_AFTER_S 90                 // untouched this long: backlight to a glow
#define DIM_PERCENT 8
#define CAL_TIMEOUT_S 60
#define SCROLL_LIMIT 25                // px a finger moves before LVGL calls it a drag: a resistive panel wobbles
#define Z_SOLID 700                    // pressure at which a point is believed (board.c says touched from 400)

typedef enum { HERS, MINE, BAD } who_t;
typedef struct { who_t who; char *text; } entry_t;
typedef struct { int entry, offset; } cut_t;     // where a page starts: an entry, a byte into it

static lv_display_t *disp;
static lv_indev_t *indev;
static lv_obj_t *bar, *dot, *menu, *title_lbl, *picker, *prev_btn, *next_btn, *page_lbl, *status_lbl;
static lv_obj_t *page, *input_row, *input, *send_btn, *keyboard, *toast;
static lv_obj_t *view;                 // the page shown instead of the chat, or NULL
static page_t current;
const char *PAGE_NAMES[PAGE_COUNT] = {"chat", "backups", "status", "board", "actions"};
static lv_timer_t *toast_timer, *clock_timer, *watch_timer;
static char status_text[48];
static bool dimmed, dark;              // dimmed comes by itself; dark is the button's "screen" job. A touch ends both
static int64_t dark_at;                // when it went dark: the tap that asked for it is not the touch that ends it
static volatile bool picture_on;       // server.c holds the glass and the LVGL lock
static bool pending_begin;             // a reply began behind the picture
static char *pending_reply;            // its text so far
static portMUX_TYPE pend_mux = portMUX_INITIALIZER_UNLOCKED;   // the three above, between the events, client and picture tasks
static bool link_on, bar_stale;        // the dot's state; the chat's name changed behind a picture

// the transcript and its pages
static entry_t entries[ENTRIES_MAX];
static int n_entries;
static int writing = -1;               // the entry of hers being written, or -1: a late piece still lands in it after done
static bool streaming;                 // her reply is arriving now: no paging into older history meanwhile
static cut_t cuts[PAGES_MAX + 1];      // cuts[n_pages] is the end
static int n_pages, shown;             // pages, and the one on the glass
static bool following = true;          // the view goes to the last page as she writes
static cut_t shown_a, shown_z;         // what the glass holds, so an unchanged page is not drawn again
static int shown_n = -1;
// the chat read back: a window that grows backward as you page
static int older = -1;                 // the cursor for the next older batch; 0 = nothing older; -1 = nothing read back yet
static bool loading, trimmed_newest;   // a batch is on its way; the newest end was dropped to make room (a tap on the page count reloads it)
static bool batch_fresh;
static int batch_added;
static cut_t anchor;                   // where the view was when an older batch was asked for

// the chat picker
static char chat_name[32], brain_name[48];
static int trim = -1;
static bool picker_open;
static char *list_pending;             // a list that arrived while the picker was open

#define C_BG lv_color_hex(0x0f1216)
#define C_BAR lv_color_hex(0x171c23)
#define C_TEXT lv_color_hex(0xe8ecf1)
#define C_YOU lv_color_hex(0x8fb4e3)
#define C_DIM lv_color_hex(0x8a93a0)
#define C_OK lv_palette_main(LV_PALETTE_GREEN)
#define C_BAD lv_palette_main(LV_PALETTE_RED)

// --- the chat font ----------------------------------------------------------------------
// 14, 16, 18 or 20 px, chosen with the console's `screen font N` and kept in flash.

static const lv_font_t *chat_font(void)
{
    switch (settings.font) {
    case 14: return &lv_font_montserrat_14;
    case 18: return &lv_font_montserrat_18;
    case 20: return &lv_font_montserrat_20;
    case 22: return &lv_font_montserrat_22;
    case 24: return &lv_font_montserrat_24;
    default: return &lv_font_montserrat_16;
    }
}

// --- her words on a small font -------------------------------------------------------
// Montserrat here has ASCII, the degree sign and the bullet. The curly and the
// long dashes she writes become their plain cousins, the markdown that means
// nothing on a screen this size is dropped, and so is any emoji that reached
// the board (Sapphire turns the common ones into faces before they do; the
// joiners and the variation marks that ride with them go too).

static void fold(char *s)
{
    char *w = s;
    for (const unsigned char *r = (const unsigned char *)s; *r;) {
        if (r[0] >= 0xF0 && r[1] && r[2] && r[3]) { r += 4; continue; }                 // U+10000 and up: the emoji planes
        if (r[0] == 0xE2 && r[1] >= 0x98 && r[1] <= 0x9E && r[2]) { r += 3; continue; }   // U+2600..27BF: suns, hearts, dingbats
        if (r[0] == 0xE2 && r[1] >= 0xAC && r[1] <= 0xAF && r[2]) { r += 3; continue; }   // U+2B00..2BFF: stars, arrows
        if (r[0] == 0xE2 && r[1] == 0x80 && r[2] == 0x8D) { r += 3; continue; }           // U+200D zero-width joiner
        if (r[0] == 0xE2 && r[1] == 0x83 && r[2] == 0xA3) { r += 3; continue; }           // U+20E3 keycap
        if (r[0] == 0xEF && r[1] == 0xB8 && r[2] >= 0x80 && r[2] <= 0x8F) { r += 3; continue; }   // U+FE00..FE0F variation marks
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

// --- the transcript ---------------------------------------------------------------------
// Entries are text; the glass shows one page of them. Under LVGL's lock.

static void drop_entry(int i)
{
    free(entries[i].text);
    memmove(&entries[i], &entries[i + 1], (n_entries - i - 1) * sizeof(entry_t));
    n_entries--;
    if (writing > i) writing--;
    else if (writing == i) writing = -1;
}

// the oldest entry leaves the window; a page back reads it again (the cursor moves past it)
static void drop_oldest(void)
{
    if (older >= 0) older++;
    drop_entry(0);
}

// a copy for the window; NULL only with the heap empty, and then no entry is made
static char *keep(const char *text)
{
    char *c = strdup(text && text[0] ? text : "...");
    if (!c) c = strdup("...");
    return c;
}

static void copy_utf8(char *dst, const char *src, size_t size)
{
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= size) { n = size - 1; while (n > 0 && (src[n] & 0xC0) == 0x80) n--; }   // never end on half a character
    memcpy(dst, src, n);
    dst[n] = 0;
}

static size_t text_total(void)
{
    size_t n = 0;
    for (int i = 0; i < n_entries; i++) n += strlen(entries[i].text);
    return n;
}

static int push(who_t who, const char *text)
{
    while (n_entries && (n_entries >= ENTRIES_MAX || text_total() + strlen(text) > TEXT_MAX)) {
        int i = writing == 0 ? 1 : 0;              // never the entry being written
        if (i >= n_entries) break;
        if (i == 0) drop_oldest(); else drop_entry(i);
    }
    char *copy = keep(text);
    if (!copy) return -1;
    entries[n_entries].who = who;
    entries[n_entries].text = copy;
    return n_entries++;
}

static bool prepend(who_t who, const char *text)
{
    // room is made at the newest end, never from the entry being written
    while (n_entries && (n_entries >= ENTRIES_MAX || text_total() + strlen(text) > TEXT_MAX)) {
        if (writing == n_entries - 1) return false;
        drop_entry(n_entries - 1);
        trimmed_newest = true;
    }
    char *copy = keep(text);
    if (!copy) return false;
    memmove(&entries[1], &entries[0], n_entries * sizeof(entry_t));
    entries[0].who = who;
    entries[0].text = copy;
    n_entries++;
    if (writing >= 0) writing++;
    return true;
}

static int page_w(void) { return BOARD_W - 2 * PAD; }
static int page_h(void) { return BOARD_H - BAR_H - INPUT_H - 2 * PAD; }

// Cut the transcript into pages: a page is the lines that fit, cut where the
// label will cut them (the same breaker, font, width and spacing).
static bool cut_pages(void)                    // false: more pages than PAGES_MAX, the last would hold the rest unseen
{
    const lv_font_t *font = chat_font();
    int lh = lv_font_get_line_height(font) + LINE_SPACE, budget = page_h(), used = 0;
    lv_text_attributes_t attr = {.letter_space = 0, .line_space = LINE_SPACE, .max_width = page_w(), .text_flags = LV_TEXT_FLAG_NONE};
    n_pages = 0;
    if (n_entries) { cuts[0] = (cut_t){0, 0}; n_pages = 1; }
    for (int e = 0; e < n_entries; e++) {
        const char *t = entries[e].text;
        size_t off = 0, len = strlen(t);
        bool first_line = true;
        while (off < len) {
            uint32_t n = lv_text_get_next_line(t + off, LV_TEXT_LEN_MAX, font, NULL, &attr);
            if (n == 0) n = len - off;                              // never stand still
            int need = lh + (first_line && used > 0 ? GAP : 0);
            if (used > 0 && used + need > budget) {                 // full: this line opens the next page
                if (n_pages >= PAGES_MAX) { cuts[n_pages] = (cut_t){n_entries, 0}; return false; }
                cuts[n_pages++] = (cut_t){e, (int)off};
                used = 0;
                need = lh;
            }
            used += need;
            first_line = false;
            off += n;
        }
    }
    cuts[n_pages] = (cut_t){n_entries, 0};
    return true;
}

static void paginate(void)
{
    while (!cut_pages() && n_entries > 1 && writing != 0) drop_oldest();   // past the last page: the oldest leaves the window
}

static void draw_page_label(void)
{
    char text[16];
    if (loading) strcpy(text, "...");
    else snprintf(text, sizeof text, "%d/%d", n_pages ? shown + 1 : 0, n_pages);
    lv_label_set_text(page_lbl, text);
    bool more_back = shown > 0 || (older != 0 && !loading && !streaming);
    lv_obj_set_style_text_color(prev_btn, more_back ? C_TEXT : lv_color_hex(0x3a4049), 0);
    lv_obj_set_style_text_color(next_btn, shown < n_pages - 1 ? C_TEXT : lv_color_hex(0x3a4049), 0);
}

static void show_page(int p)
{
    if (p >= n_pages) p = n_pages - 1;
    if (p < 0) p = 0;
    shown = p;
    cut_t a = n_pages ? cuts[p] : (cut_t){0, 0}, z = n_pages ? cuts[p + 1] : (cut_t){0, 0};
    bool same = shown_n == n_pages && !memcmp(&a, &shown_a, sizeof a) && !memcmp(&z, &shown_z, sizeof z)
                && (writing < a.entry || writing > z.entry);        // the entry being written is not on it
    draw_page_label();
    if (same) return;
    lv_obj_clean(page);
    for (int e = a.entry; e <= z.entry && e < n_entries; e++) {
        size_t from = e == a.entry ? a.offset : 0;
        size_t to = e == z.entry ? z.offset : strlen(entries[e].text);
        if (to <= from) continue;
        lv_obj_t *l = lv_label_create(page);
        lv_obj_set_width(l, page_w());
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_all(l, 0, 0);
        lv_obj_set_style_text_font(l, chat_font(), 0);
        lv_obj_set_style_text_line_space(l, LINE_SPACE, 0);
        lv_obj_set_style_text_letter_space(l, 0, 0);
        lv_obj_set_style_text_color(l, entries[e].who == HERS ? C_TEXT : entries[e].who == MINE ? C_YOU : C_BAD, 0);
        char *frag = strndup(entries[e].text + from, to - from);
        if (frag) { lv_label_set_text(l, frag); free(frag); }
    }
    shown_a = a; shown_z = z; shown_n = n_pages;
}

static void relayout(void)
{
    paginate();
    show_page(following ? n_pages - 1 : shown);
}

static void turn(int by)
{
    int to = shown + by;
    if (to < 0) {
        // past the first page: the chat read back, from before what is held
        if (loading || older == 0 || streaming) return;
        loading = true;
        draw_page_label();
        link_ask_history(older);                         // -1 = nothing read back yet: start from the newest
        return;
    }
    if (to >= n_pages) return;
    following = to == n_pages - 1;
    show_page(to);
}

static void to_end(void)
{
    following = true;
    if (trimmed_newest && !streaming && !loading) { loading = true; draw_page_label(); link_ask_history(-1); return; }
    show_page(n_pages - 1);
}

// --- the input row and the keyboard ----------------------------------------------------
// The keyboard takes the page's place: bar, input row, keys.

static void keyboard_show(bool on)
{
    if (on) { lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN); }
}

static void keyboard_hide(void)
{
    keyboard_show(false);
    lv_obj_remove_state(input, LV_STATE_FOCUSED);
}

static void keyboard_show_chat(bool on) { if (current == PAGE_CHAT) keyboard_show(on); }

static void send_now(void)
{
    const char *text = lv_textarea_get_text(input);
    while (*text == ' ') text++;
    if (!*text) return;
    push(MINE, text);
    following = true;
    relayout();
    if (!link_send_text(text)) { push(BAD, "not sent: no link"); relayout(); }
    lv_textarea_set_text(input, "");
}

static void on_input(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) keyboard_show(true);
    else if (code == LV_EVENT_READY) { send_now(); keyboard_hide(); }
    else if (code == LV_EVENT_CANCEL) keyboard_hide();
}

static void on_send(lv_event_t *e) { send_now(); keyboard_hide(); }

// --- turning pages --------------------------------------------------------------------------

static void on_page_gesture(lv_event_t *e)
{
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_LEFT) turn(1);
    else if (d == LV_DIR_RIGHT) turn(-1);
}

static void on_prev(lv_event_t *e) { turn(-1); }
static void on_next(lv_event_t *e) { turn(1); }
static void on_page_lbl(lv_event_t *e) { to_end(); }

// --- the pages and their menu --------------------------------------------------------------------

static void set_hidden(lv_obj_t *o, bool hide)
{
    if (hide) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void keyboard_hide(void);

static void show_view(page_t which)
{
    if (which >= PAGE_COUNT) which = PAGE_CHAT;
    if (view) { lv_obj_delete(view); view = NULL; }
    keyboard_hide();
    bool chat = which == PAGE_CHAT;
    set_hidden(page, !chat);
    set_hidden(input_row, !chat);
    set_hidden(picker, !chat);
    set_hidden(prev_btn, !chat);
    set_hidden(next_btn, !chat);
    set_hidden(page_lbl, !chat);
    set_hidden(title_lbl, chat);
    if (!chat) {
        static const char *titles[PAGE_COUNT] = {"", "Backups", "Status", "Board", "Actions"};
        lv_label_set_text(title_lbl, titles[which]);
        view = which == PAGE_BACKUPS ? page_backups_build(lv_screen_active()) : which == PAGE_STATUS ? page_status_build(lv_screen_active())
             : which == PAGE_BOARD ? page_board_build(lv_screen_active()) : page_actions_build(lv_screen_active());
        lv_obj_move_to_index(view, 1);                     // under the bar, above the (hidden) chat
        lv_obj_set_flex_grow(view, 1);
    }
    current = which;
    lv_dropdown_set_selected(menu, which);
    lv_display_trigger_activity(disp);
}

static void on_menu(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) show_view((page_t)lv_dropdown_get_selected(menu));
}

// --- the chat picker ---------------------------------------------------------------------

static void select_in_list(void)
{
    if (!chat_name[0]) return;
    int32_t at = lv_dropdown_get_option_index(picker, chat_name);
    if (at >= 0) lv_dropdown_set_selected(picker, at);
}

static void set_chat(const char *name)
{
    copy_utf8(chat_name, name, sizeof chat_name);
    lv_dropdown_set_text(picker, chat_name[0] ? chat_name : "chat");
    select_in_list();
}

static void apply_list(const char *joined)
{
    lv_dropdown_set_options(picker, joined && joined[0] ? joined : "");
    select_in_list();
}

static void on_picker(lv_event_t *e)
{
    // A list that arrives while the picker is open waits for the NEXT open, never
    // the close: LVGL closes the list from inside its release handler, before it
    // reads the picked index, and setting options resets that index (every pick
    // came out as the old chat, 2026-10-15)
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        if (list_pending) { apply_list(list_pending); free(list_pending); list_pending = NULL; }
        picker_open = true;
        link_ask_chats();                                    // and a fresh one for next time
    }
    else if (code == LV_EVENT_CANCEL) picker_open = false;
    else if (code == LV_EVENT_VALUE_CHANGED) {
        char name[64];
        lv_dropdown_get_selected_str(picker, name, sizeof name);
        if (name[0] && strcmp(name, chat_name)) { set_chat(name); link_pick_chat(name); }   // Sapphire confirms through /screen; link.c reads the chat back
    }
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

// BOOT's jobs (buttons.h). They run inside on_watch, on LVGL's task: no lock to take.
// Left to the board, a tap brings the keyboard and a hold clears the window.

static void job_keyboard(void)
{
    if (dimmed || dark) { dimmed = dark = false; board_backlight(settings.brightness); lv_display_trigger_activity(disp); }
    else if (current != PAGE_CHAT) show_view(PAGE_CHAT);
    else if (lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN)) { keyboard_show(true); lv_obj_add_state(input, LV_STATE_FOCUSED); }
    else keyboard_hide();
}

static void job_clear(void)
{
    while (n_entries) drop_entry(0);
    writing = -1;
    streaming = false;
    following = true;
    older = -1;                        // paging back reads the chat again
    trimmed_newest = false;
    relayout();
}

static void job_screen(void)
{
    dark = !dark;
    dimmed = false;
    dark_at = esp_timer_get_time();
    board_backlight(dark ? 0 : settings.brightness);
    if (!dark) lv_display_trigger_activity(disp);
}

static void job_backup(void) { link_ask("backup"); }
static void job_torch(void) { light_torch(!light_torch_on()); }
static void job_chat(void) { show_view(PAGE_CHAT); }

static const button_job_t JOBS[] = {
    {"keyboard", "Show or hide the keyboard", job_keyboard},
    {"clear", "Clear the chat window", job_clear},
    {"screen", "Screen on or off", job_screen},
    {"backup", "Back up onto the card now", job_backup},
    {"flashlight", "Flashlight on or off", job_torch},
    {"chat", "Open the chat page", job_chat},
};
static const button_def_t BUTTONS[] = {
    {"boot", BOARD_BOOT_BUTTON, {"keyboard", "clear", NULL}},
    // the nine on the Actions page: a tap each, nothing of the board's own; Settings > Devices sets them
    {"a", BUTTON_TAP, {NULL, NULL, NULL}}, {"b", BUTTON_TAP, {NULL, NULL, NULL}}, {"c", BUTTON_TAP, {NULL, NULL, NULL}},
    {"d", BUTTON_TAP, {NULL, NULL, NULL}}, {"e", BUTTON_TAP, {NULL, NULL, NULL}}, {"f", BUTTON_TAP, {NULL, NULL, NULL}},
    {"g", BUTTON_TAP, {NULL, NULL, NULL}}, {"h", BUTTON_TAP, {NULL, NULL, NULL}}, {"i", BUTTON_TAP, {NULL, NULL, NULL}},
};

static void on_watch(lv_timer_t *t)
{
    uint32_t idle = lv_display_get_inactive_time(disp);
    if (dark) { if (idle < 1000 && esp_timer_get_time() - dark_at > 1500000) { dark = false; board_backlight(settings.brightness); } }
    else if (!dimmed && idle > DIM_AFTER_S * 1000) { dimmed = true; board_backlight(DIM_PERCENT); }
    else if (dimmed && idle < 1000) { dimmed = false; board_backlight(settings.brightness); }
    buttons_poll();
}

static void on_toast_end(lv_timer_t *t)
{
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(toast_timer);
    toast_timer = NULL;
}

// --- the touch chip as LVGL's pointer --------------------------------------------------------
// A resistive panel lies as a finger lands and as it leaves: the first sample
// of a press can be tens of pixels off, the last one too, with the pressure
// still ramping. So a press is reported from its second sample on, a point is
// taken only at solid pressure, and the point of the lift-off is the last
// solid one. LVGL then sees a tap where the finger rested, not where it skidded.

static void touch_read(lv_indev_t *dev, lv_indev_data_t *data)
{
    static int lx, ly, seen, x0, y0;
    int x, y, z;
    bool down = board_touch_raw(NULL, NULL, &z) && board_touch(&x, &y);
    if (down) {
        if (z >= Z_SOLID) { lx = x; ly = y; }
        if (++seen == 2) { x0 = lx; y0 = ly; }
    } else if (seen) {
        if (seen >= 2) ESP_LOGI(TAG, "tap %d,%d -> %d,%d over %d samples", x0, y0, lx, ly, seen);
        seen = 0;
    }
    data->state = seen >= 2 ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = lx;
    data->point.y = ly;
}

// --- the screen ----------------------------------------------------------------------

static lv_obj_t *bar_button(const char *symbol, lv_event_cb_t cb, int x)
{
    lv_obj_t *l = lv_label_create(bar);
    lv_label_set_text(l, symbol);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, C_TEXT, 0);
    lv_obj_set_style_pad_all(l, 4, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(l, 10);
    lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(l, LV_ALIGN_CENTER, x, 0);
    return l;
}

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

    // the bar: the dot, the chat picker, the page turner, the clock
    bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), BAR_H);
    lv_obj_set_style_bg_color(bar, C_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    dot = lv_obj_create(bar);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, 3, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, C_BAD, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
    menu = lv_dropdown_create(bar);                      // the pages, behind a list symbol
    lv_obj_set_size(menu, 30, 24);
    lv_obj_set_style_text_font(menu, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(menu, C_BAR, 0);
    lv_obj_set_style_border_width(menu, 0, 0);
    lv_obj_set_style_pad_all(menu, 3, 0);
    lv_obj_set_style_text_color(menu, C_TEXT, 0);
    lv_dropdown_set_options(menu, "Chat\nBackups\nStatus\nBoard\nActions");
    lv_dropdown_set_text(menu, "");
    lv_dropdown_set_symbol(menu, LV_SYMBOL_LIST);
    lv_obj_align(menu, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(menu, on_menu, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *menu_list = lv_dropdown_get_list(menu);
    lv_obj_set_style_text_font(menu_list, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_ver(menu_list, 6, 0);
    lv_obj_set_width(menu_list, 150);
    title_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_lbl, C_TEXT, 0);
    lv_label_set_text(title_lbl, "");
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 46, 0);
    picker = lv_dropdown_create(bar);
    lv_obj_set_size(picker, 116, 24);
    lv_obj_set_style_text_font(picker, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(picker, C_BAR, 0);
    lv_obj_set_style_border_width(picker, 0, 0);
    lv_obj_set_style_pad_ver(picker, 3, 0);
    lv_obj_set_style_pad_hor(picker, 4, 0);
    lv_obj_set_style_text_color(picker, C_TEXT, 0);
    lv_dropdown_set_options(picker, "");
    lv_dropdown_set_text(picker, "chat");
    lv_obj_align(picker, LV_ALIGN_LEFT_MID, 44, 0);
    lv_obj_add_event_cb(picker, on_picker, LV_EVENT_ALL, NULL);
    lv_obj_t *list = lv_dropdown_get_list(picker);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_16, 0);
    lv_obj_set_style_max_height(list, BOARD_H - BAR_H - 4, 0);
    lv_obj_set_style_pad_ver(list, 6, 0);
    prev_btn = bar_button(LV_SYMBOL_LEFT, on_prev, 22);
    page_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(page_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(page_lbl, C_DIM, 0);
    lv_obj_set_style_pad_all(page_lbl, 4, 0);
    lv_obj_add_flag(page_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(page_lbl, 6);
    lv_obj_add_event_cb(page_lbl, on_page_lbl, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(page_lbl, "0/0");
    lv_obj_align(page_lbl, LV_ALIGN_CENTER, 54, 0);
    next_btn = bar_button(LV_SYMBOL_RIGHT, on_next, 86);
    status_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(status_lbl, C_DIM, 0);
    lv_obj_set_width(status_lbl, 60);
    lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(status_lbl, "");
    lv_obj_align(status_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    // the page: a column of labels, never scrolled; a swipe turns it
    page = lv_obj_create(scr);
    lv_obj_remove_style_all(page);
    lv_obj_set_width(page, LV_PCT(100));
    lv_obj_set_flex_grow(page, 1);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, PAD, 0);
    lv_obj_set_style_pad_row(page, GAP, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(page, on_page_gesture, LV_EVENT_GESTURE, NULL);

    // the input row
    lv_obj_t *row = input_row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), INPUT_H);
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

    // the keyboard, where the page was
    keyboard = lv_keyboard_create(scr);
    lv_obj_set_width(keyboard, LV_PCT(100));
    lv_obj_set_flex_grow(keyboard, 1);
    lv_keyboard_set_textarea(keyboard, input);
    lv_keyboard_set_popovers(keyboard, false);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_16, 0);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    // a line across the top for what she shows
    toast = lv_label_create(lv_layer_top());
    lv_obj_set_width(toast, LV_PCT(100));
    lv_label_set_long_mode(toast, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_color(toast, lv_palette_darken(LV_PALETTE_BLUE, 3), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(toast, 8, 0);
    lv_obj_set_style_text_color(toast, C_TEXT, 0);
    lv_obj_set_style_text_align(toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, BAR_H);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

    relayout();
    show_view((page_t)settings.home);
    clock_timer = lv_timer_create(on_clock, 10000, NULL);
    buttons_setup(BUTTONS, sizeof BUTTONS / sizeof BUTTONS[0], JOBS, sizeof JOBS / sizeof JOBS[0]);
    watch_timer = lv_timer_create(on_watch, 40, NULL);      // also acts on the button (buttons.c reads the pin itself)
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
        .rotation = {.swap_xy = true, .mirror_x = b->flip != b->mirror, .mirror_y = b->flip},   // as board.c turns the panel
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    disp = lvgl_port_add_disp(&dcfg);
    lvgl_port_lock(0);
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
    lv_indev_set_display(indev, disp);
    lv_indev_set_scroll_limit(indev, SCROLL_LIMIT);
    build();
    lvgl_port_unlock();
    board_backlight(settings.brightness);
    ESP_LOGI(TAG, "screen up, %d px a page, font %d", page_h(), settings.font);
}

// --- from other tasks ------------------------------------------------------------------

void ui_status(const char *text)
{
    if (!disp) return;
    strlcpy(status_text, text ? text : "", sizeof status_text);
    if (picture_on) return;
    lvgl_port_lock(0);
    draw_status();
    lvgl_port_unlock();
}

void ui_link(bool connected)
{
    link_on = connected;                   // drawn when the picture goes, if one is up
    if (!disp || picture_on) return;
    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(dot, connected ? C_OK : C_BAD, 0);
    lvgl_port_unlock();
}

// Her reply. While server.c holds the glass (picture_on), what arrives is kept
// in pending_* and drawn when the picture goes: three tasks touch those two
// (events, client, picture), so the swaps are under a spinlock, and the
// picture task empties them before it lets go of picture_on, so nothing is
// stranded behind it (a double free was possible there, 2026-10-16).

static void reply_begin_now(void)
{
    lvgl_port_lock(0);
    writing = push(HERS, "...");
    streaming = true;
    following = true;
    relayout();
    lvgl_port_unlock();
}

static void reply_set_now(const char *text)
{
    lvgl_port_lock(0);
    if (writing < 0) writing = push(HERS, "...");
    char *copy = writing < 0 ? NULL : strndup(text ? text : "", REPLY_MAX + 4);
    if (copy) {
        if (strlen(copy) > REPLY_MAX) strcpy(copy + REPLY_MAX, "...");
        fold(copy);
        char *next = *copy ? copy : keep("...");
        if (!*copy) free(copy);
        if (next) { free(entries[writing].text); entries[writing].text = next; relayout(); }
    }
    lvgl_port_unlock();
}

void ui_reply_begin(void)
{
    if (!disp) return;
    if (picture_on) {
        char *old = NULL;
        taskENTER_CRITICAL(&pend_mux);
        bool behind = picture_on;
        if (behind) { pending_begin = true; old = pending_reply; pending_reply = NULL; }
        taskEXIT_CRITICAL(&pend_mux);
        free(old);
        if (behind) return;
    }
    reply_begin_now();
}

void ui_reply_set(const char *text)
{
    if (!disp) return;
    if (picture_on) {
        char *fresh = strdup(text ? text : ""), *old = NULL;
        taskENTER_CRITICAL(&pend_mux);
        bool behind = picture_on;
        if (behind) { old = pending_reply; pending_reply = fresh; }
        taskEXIT_CRITICAL(&pend_mux);
        free(behind ? old : fresh);
        if (behind) return;
    }
    reply_set_now(text);
}

void ui_reply_done(void)
{
    // the entry stays the current one: a last piece that lands late still goes
    // in it, not in a new entry. Paging into older history is open again.
    streaming = false;
    if (!disp || picture_on) return;
    lvgl_port_lock(0);
    if (writing >= 0 && !strcmp(entries[writing].text, "...")) {
        char *t = keep("(no answer)");
        if (t) { free(entries[writing].text); entries[writing].text = t; }
    }
    relayout();                            // the prev arrow lights again
    lvgl_port_unlock();
}

void ui_failed(const char *why)
{
    if (!disp || picture_on) return;
    lvgl_port_lock(0);
    push(BAD, why);
    following = true;
    relayout();
    lvgl_port_unlock();
}

void ui_mine(const char *text)
{
    if (!disp || picture_on) return;
    lvgl_port_lock(0);
    char *copy = strndup(text ? text : "", 300);
    if (copy) { fold(copy); push(MINE, copy); free(copy); following = true; relayout(); }
    lvgl_port_unlock();
}

void ui_toast(const char *text, int seconds)
{
    if (!disp || picture_on) return;
    lvgl_port_lock(0);
    if (!text || !text[0]) {                                   // "" = whatever is up goes
        lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
        if (toast_timer) { lv_timer_delete(toast_timer); toast_timer = NULL; }
        lvgl_port_unlock();
        return;
    }
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
    if (dimmed || dark) { dimmed = dark = false; board_backlight(settings.brightness); }
    if (disp) lv_display_trigger_activity(disp);
}

void ui_repaint(void)
{
    if (!disp || picture_on) return;
    lvgl_port_lock(0);
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lvgl_port_unlock();
}

bool ui_chat(const char *name, const char *brain, int trim_rgb)
{
    if (!disp) return false;
    char want[sizeof chat_name];
    copy_utf8(want, name, sizeof want);
    bool changed = strcmp(want, chat_name) != 0;
    if (picture_on) {                      // the glass is held by server.c: the names are kept, the bar is drawn when it lets go
        copy_utf8(brain_name, brain, sizeof brain_name);
        trim = trim_rgb;
        copy_utf8(chat_name, name, sizeof chat_name);
        bar_stale = true;
        return changed;
    }
    lvgl_port_lock(0);
    copy_utf8(brain_name, brain, sizeof brain_name);
    trim = trim_rgb;
    set_chat(name);
    lvgl_port_unlock();
    return changed;
}

void ui_history_begin(bool fresh)
{
    if (!disp) return;
    lvgl_port_lock(0);
    batch_fresh = fresh;
    batch_added = 0;
    if (fresh) {
        while (n_entries) drop_entry(0);
        writing = -1;
        streaming = false;
        trimmed_newest = false;
        following = true;
    } else {
        anchor = n_pages ? cuts[shown] : (cut_t){0, 0};
    }
    lvgl_port_unlock();
}

void ui_history_add(bool hers, const char *text)
{
    if (!disp) return;
    lvgl_port_lock(0);
    char *copy = strndup(text ? text : "", REPLY_MAX + 4);
    if (copy) {
        if (strlen(copy) > REPLY_MAX) strcpy(copy + REPLY_MAX, "...");
        fold(copy);
        if (prepend(hers ? HERS : MINE, copy)) batch_added++;      // newest first in: each goes before the last
        free(copy);
    }
    lvgl_port_unlock();
}

void ui_history_end(int before)
{
    if (!disp) return;
    lvgl_port_lock(0);
    older = before;
    loading = false;
    paginate();
    shown_n = -1;                                        // other entries under the same cuts: draw the page again
    if (batch_fresh) show_page(n_pages - 1);
    else {
        // the view stays on the line it was on: that cut moved down by what came in
        cut_t at = {anchor.entry + batch_added, anchor.offset};
        int p = 0;
        for (int i = 0; i < n_pages; i++)
            if (cuts[i].entry < at.entry || (cuts[i].entry == at.entry && cuts[i].offset <= at.offset)) p = i;
        following = p == n_pages - 1;
        show_page(p);
    }
    lvgl_port_unlock();
}

void ui_history_fail(void)
{
    if (!disp) return;
    lvgl_port_lock(0);
    loading = false;
    draw_page_label();
    lvgl_port_unlock();
}

const char *ui_chat_name(void) { return chat_name; }
const char *ui_brain_name(void) { return brain_name; }
bool ui_up(void) { return disp != NULL; }
void ui_copy_text(char *dst, const char *src, size_t size) { copy_utf8(dst, src, size); }

void ui_show_page(page_t which)
{
    if (!disp || picture_on) return;
    lvgl_port_lock(0);
    show_view(which);
    lvgl_port_unlock();
}

bool ui_set_home(const char *name)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (name && !strcmp(name, PAGE_NAMES[i])) {
            settings.home = i;
            settings_save();
            return true;
        }
    }
    return false;
}

const char *ui_home_name(void) { return PAGE_NAMES[settings.home < PAGE_COUNT ? settings.home : 0]; }

void ui_dark(void)
{
    dark = true;
    dimmed = false;
    dark_at = esp_timer_get_time();
    board_backlight(0);
}

void ui_button_labels(const cJSON *labels)
{
    if (!disp) return;
    lvgl_port_lock(0);
    static const char *names[VBUTTONS] = {"a", "b", "c", "d", "e", "f", "g", "h", "i"};
    for (int i = 0; i < VBUTTONS; i++) {
        const cJSON *w = cJSON_IsObject(labels) ? cJSON_GetObjectItem(labels, names[i]) : NULL;
        page_actions_label(i, cJSON_IsString(w) ? w->valuestring : "");
    }
    lvgl_port_unlock();
}

void ui_chats(const char *joined, const char *current)
{
    if (!disp) return;
    lvgl_port_lock(0);
    if (picker_open) { free(list_pending); list_pending = strdup(joined ? joined : ""); }
    else apply_list(joined);
    if (current && current[0] && strcmp(current, chat_name)) set_chat(current);
    lvgl_port_unlock();
}

bool ui_picture_begin(void)
{
    if (!disp || picture_on || !lvgl_port_lock(1500)) return false;
    picture_on = true;
    if (dimmed || dark) { dimmed = dark = false; board_backlight(settings.brightness); }
    return true;
}

void ui_picture_end(void)
{
    if (!picture_on) return;
    for (;;) {                             // what came behind the picture, until nothing more does
        taskENTER_CRITICAL(&pend_mux);
        bool begin = pending_begin;
        char *text = pending_reply;
        pending_begin = false;
        pending_reply = NULL;
        if (!begin && !text) picture_on = false;
        taskEXIT_CRITICAL(&pend_mux);
        if (!begin && !text) break;
        if (begin) reply_begin_now();
        if (text) { reply_set_now(text); free(text); }
    }
    lv_display_trigger_activity(disp);
    lv_obj_invalidate(lv_screen_active());  // everything is repainted over the picture
    lv_obj_invalidate(lv_layer_top());
    draw_status();
    lv_obj_set_style_bg_color(dot, link_on ? C_OK : C_BAD, 0);
    if (bar_stale) { bar_stale = false; set_chat(chat_name); }
    lvgl_port_unlock();
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

static void on_card_cal(lv_event_t *e)
{
    lv_obj_t *card = lv_event_get_user_data(e);
    lv_obj_delete(card);
    ui_calibrate();                    // on LVGL's task already: the lock is recursive
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
        lv_obj_set_pos(bar_, 8 + i * step, 70);
        lv_obj_remove_flag(bar_, LV_OBJ_FLAG_CLICKABLE);          // a tap on a bar is a tap on the card
        lv_obj_t *l = lv_label_create(card);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_label_set_text(l, bars[i].name);
        lv_obj_set_pos(l, 8 + i * step, 70 + tall + 4);
    }
    lv_obj_t *top = lv_label_create(card);
    lv_obj_set_style_text_font(top, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(top, lv_color_white(), 0);
    lv_label_set_text(top, "TOP");
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_label_set_text(hint, "tap anywhere: the serial line says where");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_size(btn, 110, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 40, -12);
    lv_obj_add_event_cb(btn, on_card_back, LV_EVENT_CLICKED, card);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "done");
    lv_obj_center(bl);
    lv_obj_t *cal = lv_button_create(card);
    lv_obj_set_size(cal, 110, 44);
    lv_obj_align(cal, LV_ALIGN_BOTTOM_RIGHT, -40, -12);
    lv_obj_add_event_cb(cal, on_card_cal, LV_EVENT_CLICKED, card);
    lv_obj_t *cl = lv_label_create(cal);
    lv_label_set_text(cl, "calibrate");
    lv_obj_center(cl);
    ui_wake();
    lvgl_port_unlock();
}

// --- calibration: three crosses --------------------------------------------------------------
// Tap the cross at the top-left, the top-right, then the bottom-right. The
// chip is read raw while a finger is down and the tap's mean is the point;
// board.c keeps the result. CAL lines go to the serial line for setup_board.py.

static lv_obj_t *cal_card, *cal_h, *cal_v, *cal_hint;
static lv_timer_t *cal_timer;
static int cal_step, cal_n, cal_ticks, cal_raw[3][2];
static long cal_sum[2];
static const int CAL_AT[3][2] = {{CAL_INSET, CAL_INSET}, {BOARD_W - 1 - CAL_INSET, CAL_INSET}, {BOARD_W - 1 - CAL_INSET, BOARD_H - 1 - CAL_INSET}};

static void cal_place(void)
{
    lv_obj_set_pos(cal_h, CAL_AT[cal_step][0] - 12, CAL_AT[cal_step][1] - 1);
    lv_obj_set_pos(cal_v, CAL_AT[cal_step][0] - 1, CAL_AT[cal_step][1] - 12);
    static const char *where[3] = {"top left", "top right", "bottom right"};
    char text[64];
    snprintf(text, sizeof text, "tap the cross: %s (%d of 3)", where[cal_step], cal_step + 1);
    lv_label_set_text(cal_hint, text);
}

static void cal_end(const char *how)
{
    lv_timer_delete(cal_timer);
    cal_timer = NULL;
    lv_obj_delete(cal_card);
    cal_card = NULL;
    printf("CAL %s\n", how);
    fflush(stdout);
}

static void on_cal_tick(lv_timer_t *t)
{
    int x, y, z;
    if (++cal_ticks > CAL_TIMEOUT_S * 1000 / 30) { cal_end("timeout"); return; }
    if (board_touch_raw(&x, &y, &z)) {
        if (++cal_n > 2) { cal_sum[0] += x; cal_sum[1] += y; }     // the first samples of a press skid: not counted
        return;
    }
    if (cal_n >= 6) {                  // a finger was down and is up: a tap
        cal_raw[cal_step][0] = cal_sum[0] / (cal_n - 2);
        cal_raw[cal_step][1] = cal_sum[1] / (cal_n - 2);
        printf("CAL point %d raw %d %d\n", cal_step + 1, cal_raw[cal_step][0], cal_raw[cal_step][1]);
        fflush(stdout);
        if (++cal_step == 3) { cal_end(board_touch_calibrate(cal_raw) ? "done" : "refused: two taps on one spot, the old calibration stays"); return; }
        cal_place();
    }
    cal_sum[0] = cal_sum[1] = 0;
    cal_n = 0;
}

void ui_calibrate(void)
{
    if (!disp) return;
    lvgl_port_lock(0);
    if (cal_card) { lvgl_port_unlock(); return; }
    cal_card = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cal_card);
    lv_obj_set_size(cal_card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cal_card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cal_card, LV_OPA_COVER, 0);
    lv_obj_add_flag(cal_card, LV_OBJ_FLAG_CLICKABLE);         // taps stop here
    cal_h = lv_obj_create(cal_card);
    lv_obj_remove_style_all(cal_h);
    lv_obj_set_size(cal_h, 25, 3);
    lv_obj_set_style_bg_color(cal_h, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cal_h, LV_OPA_COVER, 0);
    cal_v = lv_obj_create(cal_card);
    lv_obj_remove_style_all(cal_v);
    lv_obj_set_size(cal_v, 3, 25);
    lv_obj_set_style_bg_color(cal_v, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cal_v, LV_OPA_COVER, 0);
    cal_hint = lv_label_create(cal_card);
    lv_obj_set_style_text_font(cal_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(cal_hint, lv_color_white(), 0);
    lv_obj_align(cal_hint, LV_ALIGN_CENTER, 0, 0);
    cal_step = cal_n = cal_ticks = 0;
    cal_sum[0] = cal_sum[1] = 0;
    cal_place();
    cal_timer = lv_timer_create(on_cal_tick, 30, NULL);
    ui_wake();
    lvgl_port_unlock();
}
