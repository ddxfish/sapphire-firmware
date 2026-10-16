// pages.c - see pages.h.
#include "pages.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"   // every snprintf here cuts on purpose: a small screen

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "buttons.h"
#include "light.h"
#include "link.h"
#include "net.h"
#include "ota.h"
#include "settings.h"
#include "speaker.h"
#include "storage.h"
#include "ui.h"

#define C_TEXT lv_color_hex(0xe8ecf1)
#define C_DIM lv_color_hex(0x8a93a0)
#define C_PANEL lv_color_hex(0x171c23)
#define C_BAD lv_palette_main(LV_PALETTE_RED)

// --- bits every page uses ---------------------------------------------------------------------

static lv_obj_t *column(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);     // never the widget default (130 px: it cut the pages off, 2026-10-15)
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);           // taller than the glass = it scrolls (an offset, no RAM)
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(c, C_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(c, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(c, 2, LV_PART_SCROLLBAR);
    return c;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_line_space(l, 3, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *words, lv_event_cb_t cb, void *arg, int w, int h)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_label_set_text(l, words);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    return b;
}

static lv_obj_t *grid(lv_obj_t *parent)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_width(g, LV_PCT(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(g, 8, 0);
    lv_obj_set_style_pad_column(g, 6, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    return g;
}

static void gb(uint64_t bytes, char *out, size_t size)
{
    snprintf(out, size, "%.1f GB", bytes / 1073741824.0);
}

static void backup_words(const char *name, char *out, size_t size);   // below, with the backups page

// --- status --------------------------------------------------------------------------------------
// Rows: a dim word on the left, the value on the right. Refreshed every two seconds. Nine rows
// of 14 px type fill the glass under the bar exactly once; a value that wraps makes the page scroll.

enum { ST_CHAT, ST_MODEL, ST_LINK, ST_WIFI, ST_RAM, ST_UP, ST_CARD, ST_LIGHT, ST_SCREEN, ST_COUNT };
static const char *ST_KEYS[ST_COUNT] = {"chat", "model", "link", "WiFi", "RAM", "up", "card", "light", "screen"};
static lv_obj_t *st_val[ST_COUNT];
static lv_timer_t *st_timer;

static lv_obj_t *kv(lv_obj_t *parent, const char *key)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_width(k, 64);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(k, C_DIM, 0);
    lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(k, key);
    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(v, C_TEXT, 0);
    lv_label_set_text(v, "");
    return v;
}

static void status_fill(void)
{
    char words[128], f2[24];
    storage_state_t st;
    storage_state(&st);
    uint64_t free_b = 0, total = 0;
    int64_t up = esp_timer_get_time() / 1000000;
    lv_label_set_text(st_val[ST_CHAT], ui_chat_name()[0] ? ui_chat_name() : "none");
    lv_label_set_text(st_val[ST_MODEL], ui_brain_name()[0] ? ui_brain_name() : "not told yet");
    snprintf(words, sizeof words, "%s, %s", link_connected() ? "linked" : "no link", net_ip()[0] ? net_ip() : "no address");
    lv_label_set_text(st_val[ST_LINK], words);
    snprintf(words, sizeof words, "%d dBm, %s", net_rssi(), settings.wifi_ssid[0] ? settings.wifi_ssid : "not set");
    lv_label_set_text(st_val[ST_WIFI], words);
    snprintf(words, sizeof words, "%u KB free, largest %u KB", (unsigned)esp_get_free_heap_size() / 1024,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) / 1024);
    lv_label_set_text(st_val[ST_RAM], words);
    snprintf(words, sizeof words, "%lldh %02lldm", (long long)(up / 3600), (long long)((up / 60) % 60));
    lv_label_set_text(st_val[ST_UP], words);
    if (st.mounted && storage_room(&free_b, &total)) {
        if (st.count) snprintf(f2, sizeof f2, "%d backup%s", st.count, st.count == 1 ? "" : "s");
        else strcpy(f2, "no backups yet");
        snprintf(words, sizeof words, "%.1f of %.1f GB free, %s", free_b / 1073741824.0, total / 1073741824.0, f2);
    } else strcpy(words, "none");
    lv_label_set_text(st_val[ST_CARD], words);
    snprintf(words, sizeof words, "%d (dark reads high)", board_light_raw());
    lv_label_set_text(st_val[ST_LIGHT], words);
    snprintf(words, sizeof words, "%s, %d%% bright, font %d%s", board_panel_name(), settings.brightness, settings.font,
             board_touch_needs_cal() && !strncmp(board_touch_words(), "NOT", 3) ? ", touch NOT calibrated" : "");
    lv_label_set_text(st_val[ST_SCREEN], words);
}

static void on_status_tick(lv_timer_t *t) { status_fill(); }

static void on_status_gone(lv_event_t *e)
{
    if (st_timer) lv_timer_delete(st_timer);
    st_timer = NULL;
    memset(st_val, 0, sizeof st_val);
}

lv_obj_t *page_status_build(lv_obj_t *parent)
{
    lv_obj_t *c = column(parent);
    lv_obj_set_style_pad_row(c, 2, 0);
    lv_obj_t *head = text(c, &lv_font_montserrat_16, C_TEXT);
    char words[96];
    snprintf(words, sizeof words, "%s   #8a93a0 %s %s, %s#", settings.name, BOARD_NAME, PROJECT_VER, ota_slot());
    lv_label_set_recolor(head, true);
    lv_label_set_text(head, words);
    for (int i = 0; i < ST_COUNT; i++) st_val[i] = kv(c, ST_KEYS[i]);
    status_fill();
    st_timer = lv_timer_create(on_status_tick, 2000, NULL);
    lv_obj_add_event_cb(c, on_status_gone, LV_EVENT_DELETE, NULL);
    return c;
}

// --- backups -------------------------------------------------------------------------------------
// The newest ten on the card, each with a delete (asked twice), the card's
// room, a Back up now that turns into Cancel while one lands.

static lv_obj_t *bk_head, *bk_rows, *bk_bar, *bk_prog, *bk_btn, *bk_ask;
static lv_timer_t *bk_timer;
static int bk_count = -1;
static bool bk_writing;
static char bk_names[10][STORAGE_NAME_MAX + 1];   // the rows shown, for their delete buttons
static int bk_n;

// "sapphire_2026-10-15_214832_manual.sapphirebak" -> "2026-10-15 21:48  manual"
static void backup_words(const char *name, char *out, size_t size)
{
    const char *p = strncmp(name, "sapphire_", 9) ? name : name + 9;
    char date[11] = {0}, clock[7] = {0}, kind[24] = {0};
    if (strlen(p) > 18 && p[10] == '_' && sscanf(p, "%10[0-9-]_%6[0-9]_%23[^.]", date, clock, kind) >= 2)
        snprintf(out, size, "%s %.2s:%.2s  %s", date, clock, clock + 2, kind);
    else snprintf(out, size, "%.40s", name);
}

static void ask_close(void)
{
    if (bk_ask) { lv_obj_delete(bk_ask); bk_ask = NULL; }
}

static void on_ask_keep(lv_event_t *e) { ask_close(); }

static void on_ask_delete(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ask_close();
    if (i < 0 || i >= bk_n) return;
    if (!storage_remove(bk_names[i])) { ui_toast("the card would not let go of it", 4); return; }
    bk_count = -1;                     // the list is drawn again on the next tick
}

// the X on a row: a sheet over everything, the question in the middle, Delete or Keep
static void on_row_delete(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (bk_ask || i < 0 || i >= bk_n) return;
    bk_ask = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bk_ask);
    lv_obj_set_size(bk_ask, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(bk_ask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bk_ask, LV_OPA_70, 0);
    lv_obj_add_flag(bk_ask, LV_OBJ_FLAG_CLICKABLE);   // taps stop here
    lv_obj_t *box = lv_obj_create(bk_ask);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, C_PANEL, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 10, 0);
    lv_obj_center(box);
    lv_obj_t *q = text(box, &lv_font_montserrat_16, C_TEXT);
    char when[80], words[128];
    backup_words(bk_names[i], when, sizeof when);
    snprintf(words, sizeof words, "Delete the backup of %s from the card?", when);
    lv_label_set_text(q, words);
    lv_obj_t *row = grid(box);
    lv_obj_t *del = button(row, "Delete", on_ask_delete, (void *)(intptr_t)i, 110, 40);
    lv_obj_set_style_bg_color(del, C_BAD, 0);
    button(row, "Keep", on_ask_keep, NULL, 110, 40);
}

static void backups_list(void)
{
    struct { char name[STORAGE_NAME_MAX + 1]; long mtime; uint64_t size; } rows[10];
    int n = 0;
    DIR *d = storage_ok() ? opendir(storage_dir()) : NULL;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t len = strlen(e->d_name);
            if (len < 13 || strcmp(e->d_name + len - 12, ".sapphirebak")) continue;
            char path[STORAGE_NAME_MAX + 64];
            struct stat s;
            if (!storage_path(e->d_name, path, sizeof path) || stat(path, &s)) continue;
            int at = n < 10 ? n : 9;            // keep the ten newest: insert sorted, drop the oldest
            if (n == 10 && (long)s.st_mtime <= rows[9].mtime) continue;
            while (at > 0 && rows[at - 1].mtime < (long)s.st_mtime) { if (at < 10) rows[at] = rows[at - 1]; at--; }
            strlcpy(rows[at].name, e->d_name, sizeof rows[at].name);
            rows[at].mtime = s.st_mtime;
            rows[at].size = s.st_size;
            if (n < 10) n++;
        }
        closedir(d);
    }
    ask_close();                                           // the rows it named are being drawn again
    lv_obj_clean(bk_rows);
    bk_n = n;
    for (int i = 0; i < n; i++) {
        strlcpy(bk_names[i], rows[i].name, sizeof bk_names[i]);
        lv_obj_t *r = lv_obj_create(bk_rows);
        lv_obj_remove_style_all(r);
        lv_obj_set_width(r, LV_PCT(100));
        lv_obj_set_height(r, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 8, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *l = lv_label_create(r);
        lv_obj_set_flex_grow(l, 1);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(l, C_DIM, 0);
        char words[80], one[112];
        backup_words(rows[i].name, words, sizeof words);
        snprintf(one, sizeof one, "%s   %llu MB", words, (unsigned long long)(rows[i].size >> 20));
        lv_label_set_text(l, one);
        button(r, LV_SYMBOL_CLOSE, on_row_delete, (void *)(intptr_t)i, 34, 28);
    }
    if (!n) {
        lv_obj_t *l = text(bk_rows, &lv_font_montserrat_14, C_DIM);
        lv_label_set_text(l, storage_ok() ? "No backups on the card yet." : "");
    }
}

static void backups_fill(void)
{
    storage_state_t st;
    storage_state(&st);
    uint64_t free_b = 0, total = 0;
    char head[96], f1[16], f2[16];
    if (st.mounted && storage_room(&free_b, &total)) {
        gb(free_b, f1, sizeof f1);
        gb(total, f2, sizeof f2);
        snprintf(head, sizeof head, "Card: %s free of %s", f1, f2);
    } else strcpy(head, "No card in the slot.");
    lv_label_set_text(bk_head, head);
    if (st.writing) {
        int pct = st.expect ? (int)(st.got * 100 / st.expect) : 0;
        char words[64];
        snprintf(words, sizeof words, "landing: %llu of %llu MB", (unsigned long long)(st.got >> 20), (unsigned long long)(st.expect >> 20));
        lv_label_set_text(bk_prog, words);
        lv_bar_set_value(bk_bar, pct, LV_ANIM_OFF);
        lv_obj_remove_flag(bk_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(bk_prog, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(bk_btn, 0), "Cancel");
        lv_obj_remove_state(bk_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_flag(bk_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bk_prog, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(bk_btn, 0), "Back up now");
        if (st.mounted && link_connected()) lv_obj_remove_state(bk_btn, LV_STATE_DISABLED);
        else lv_obj_add_state(bk_btn, LV_STATE_DISABLED);
    }
    if (st.count != bk_count || (bk_writing && !st.writing)) backups_list();
    bk_count = st.count;
    bk_writing = st.writing;
}

static void on_backups_tick(lv_timer_t *t) { backups_fill(); }

static void on_backup_now(lv_event_t *e)
{
    storage_state_t st;
    storage_state(&st);
    if (st.writing) {                  // the same button says Cancel while one lands
        if (storage_cancel()) ui_toast("the backup is cancelled; what landed is dropped", 4);
        return;
    }
    link_ask("backup");                // Sapphire answers with a line; link.c shows it
    lv_obj_add_state(bk_btn, LV_STATE_DISABLED);
}

static void on_backups_gone(lv_event_t *e)
{
    ask_close();
    if (bk_timer) lv_timer_delete(bk_timer);
    bk_timer = NULL;
    bk_head = bk_rows = bk_bar = bk_prog = bk_btn = NULL;
    bk_count = -1;
    bk_n = 0;
}

lv_obj_t *page_backups_build(lv_obj_t *parent)
{
    lv_obj_t *c = column(parent);
    bk_head = text(c, &lv_font_montserrat_16, C_TEXT);
    bk_rows = lv_obj_create(c);
    lv_obj_remove_style_all(bk_rows);
    lv_obj_set_width(bk_rows, LV_PCT(100));
    lv_obj_set_height(bk_rows, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bk_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bk_rows, 4, 0);
    lv_obj_remove_flag(bk_rows, LV_OBJ_FLAG_SCROLLABLE);   // the page scrolls, not the list
    bk_bar = lv_bar_create(c);
    lv_obj_set_size(bk_bar, LV_PCT(100), 10);
    lv_bar_set_range(bk_bar, 0, 100);
    bk_prog = text(c, &lv_font_montserrat_14, C_TEXT);
    lv_obj_t *row = grid(c);
    bk_btn = button(row, "Back up now", on_backup_now, NULL, 150, 40);
    bk_count = -1;
    backups_fill();
    bk_timer = lv_timer_create(on_backups_tick, 1000, NULL);
    lv_obj_add_event_cb(c, on_backups_gone, LV_EVENT_DELETE, NULL);
    return c;
}

// --- board -----------------------------------------------------------------------------------------
// Knobs and sliders (Krem, 2026-10-16): the LED to play with, the backlight,
// how the glass sits, the chat font, her volume, a tone, and the system row.
// A slider sets its thing as it moves and the flash word once let go of.

static lv_obj_t *bd_r, *bd_g, *bd_b;
static const int FONTS[6] = {14, 16, 18, 20, 22, 24};
static const int ROTS[4] = {0, 90, 180, 270};

static void heading(lv_obj_t *parent, const char *words)
{
    lv_obj_t *l = text(parent, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(l, words);
    lv_obj_set_style_pad_top(l, 8, 0);
}

static void value_label(lv_obj_t *s)                   // the number after a slider
{
    lv_label_set_text_fmt(lv_obj_get_child(lv_obj_get_parent(s), 2), "%d", (int)lv_slider_get_value(s));
}

// a word, the slider, its value. The callback comes as it moves and once more when let go of.
static lv_obj_t *slider(lv_obj_t *parent, const char *word, int lo, int hi, int at, lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 34);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_width(k, 62);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(k, C_DIM, 0);
    lv_label_set_text(k, word);
    lv_obj_t *s = lv_slider_create(row);
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_height(s, 8);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, at, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, color, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 7, LV_PART_KNOB);      // a knob a finger finds on resistive glass
    lv_obj_set_ext_click_area(s, 12);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);
    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_width(v, 34);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(v, C_TEXT, 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text_fmt(v, "%d", at);
    return s;
}

// a row of small buttons for one choice; the one in force stays lit, the rest sit dim
static void pick(lv_obj_t *row, const char *words, bool current, lv_event_cb_t cb, int value, int w)
{
    lv_obj_t *b = button(row, words, cb, (void *)(intptr_t)value, w, 36);
    if (!current) {
        lv_obj_set_style_bg_color(b, C_PANEL, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), C_DIM, 0);
    }
}

static void restart_now(lv_timer_t *t) { esp_restart(); }

static void restart_after(const char *why)
{
    ui_toast(why, 3);
    lv_timer_set_repeat_count(lv_timer_create(restart_now, 500, NULL), 1);   // once the toast is drawn (a wait here would hold LVGL)
}

// --- light: the LED is yours while you play, hers again on "hers"
static void on_rgb(lv_event_t *e)
{
    value_label((lv_obj_t *)lv_event_get_target(e));
    light_torch_color(lv_slider_get_value(bd_r), lv_slider_get_value(bd_g), lv_slider_get_value(bd_b));
}

static void on_white(lv_event_t *e)
{
    lv_slider_set_value(bd_r, 255, LV_ANIM_OFF); value_label(bd_r);
    lv_slider_set_value(bd_g, 150, LV_ANIM_OFF); value_label(bd_g);
    lv_slider_set_value(bd_b, 110, LV_ANIM_OFF); value_label(bd_b);
    light_torch_color(255, 150, 110);              // the flashlight: this LED's warm white
}

static void on_hers(lv_event_t *e) { light_torch(false); ui_toast("the light is hers again", 2); }

// --- screen
static void on_bright(lv_event_t *e)
{
    lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
    value_label(s);
    settings.brightness = lv_slider_get_value(s);
    board_backlight(settings.brightness);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

static void on_rotate(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if (v == board()->rotation) return;
    if (storage_busy()) { ui_toast("a backup is arriving: not now", 4); return; }
    board_set_rotation(v);
    restart_after("turning the glass: restarting");
}

static void on_font(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if (v == settings.font) return;
    if (storage_busy()) { ui_toast("a backup is arriving: not now", 4); return; }
    settings.font = v;
    settings_save();
    restart_after("the new font: restarting");
}

static void on_dark(lv_event_t *e) { ui_dark(); }

// --- sound
static void on_volume(lv_event_t *e)
{
    lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
    value_label(s);
    settings.volume = lv_slider_get_value(s);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

static void on_tone(lv_event_t *e)
{
    if (!speaker_ok()) { ui_toast("no speaker on this board", 4); return; }
    speaker_tone(880, 350);
}

// --- system
static void on_reload(lv_event_t *e)
{
    ui_show_page(PAGE_CHAT);
    link_ask_history(-1);
}

static void on_cal(lv_event_t *e) { ui_calibrate(); }

static void on_restart(lv_event_t *e)
{
    if (storage_busy()) { ui_toast("a backup is arriving: not now", 4); return; }
    restart_after("restarting");
}

static void on_board_gone(lv_event_t *e) { bd_r = bd_g = bd_b = NULL; }

lv_obj_t *page_board_build(lv_obj_t *parent)
{
    lv_obj_t *c = column(parent);
    lv_obj_set_style_pad_row(c, 4, 0);
    int w = board_w();
    const uint8_t *rgb = light_torch_rgb();

    heading(c, "Light");
    bd_r = slider(c, "red", 0, 255, rgb[0], lv_palette_main(LV_PALETTE_RED), on_rgb);
    bd_g = slider(c, "green", 0, 255, rgb[1], lv_palette_main(LV_PALETTE_GREEN), on_rgb);
    bd_b = slider(c, "blue", 0, 255, rgb[2], lv_palette_main(LV_PALETTE_BLUE), on_rgb);
    lv_obj_t *g = grid(c);
    lv_obj_t *white = button(g, "Flashlight", on_white, NULL, 120, 40);
    lv_obj_set_style_bg_color(white, lv_color_hex(0xf2ede4), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(white, 0), lv_color_black(), 0);
    button(g, "Hers", on_hers, NULL, 120, 40);

    heading(c, "Screen");
    slider(c, "backlight", 10, 100, settings.brightness, lv_palette_main(LV_PALETTE_AMBER), on_bright);
    lv_obj_t *rot = grid(c);
    for (int i = 0; i < 4; i++) {
        char words[8];
        snprintf(words, sizeof words, "%d", ROTS[i]);
        pick(rot, words, ROTS[i] == board()->rotation, on_rotate, ROTS[i], (w - 16 - 18) / 4 - 4);
    }
    lv_obj_t *fonts = grid(c);
    for (int i = 0; i < 6; i++) {
        char words[8];
        snprintf(words, sizeof words, "%d", FONTS[i]);
        pick(fonts, words, FONTS[i] == settings.font, on_font, FONTS[i], (w - 16 - 30) / 6 - 2);
    }
    lv_obj_t *sg = grid(c);
    button(sg, "Screen off", on_dark, NULL, 120, 40);

    heading(c, "Sound");
    if (speaker_ok()) slider(c, "volume", 0, 100, speaker_volume(), lv_palette_main(LV_PALETTE_TEAL), on_volume);
    lv_obj_t *snd = grid(c);
    button(snd, "Tone", on_tone, NULL, 120, 40);

    heading(c, "System");
    lv_obj_t *sys = grid(c);
    button(sys, "Reload chat", on_reload, NULL, 100, 40);
    if (board_touch_needs_cal()) button(sys, "Calibrate", on_cal, NULL, 100, 40);
    lv_obj_t *rs = button(sys, "Restart", on_restart, NULL, 100, 40);
    lv_obj_set_style_bg_color(rs, C_BAD, 0);

    lv_obj_add_event_cb(c, on_board_gone, LV_EVENT_DELETE, NULL);
    return c;
}

// --- actions: six buttons of your own -----------------------------------------------------------------

static const char *VNAMES[VBUTTONS] = {"a", "b", "c", "d", "e", "f", "g", "h", "i"};
static char vlabels[VBUTTONS][VLABEL_MAX + 1];
static lv_obj_t *vbtn[VBUTTONS];

// the words on a tile: the message's first words, or the name of the board's
// job it is set to (a job carries no label from Sapphire: it read "not set"
// though it worked, 2026-10-16), else "not set". True when it is set to anything.
static bool vbutton_words(int i, char *out, size_t size)
{
    char job[BUTTON_JOB];
    if (vlabels[i][0]) { snprintf(out, size, "%s", vlabels[i]); return true; }
    if (buttons_does(VNAMES[i], PRESS_SHORT, job, sizeof job)) { snprintf(out, size, "%c\n%s", 'A' + i, !strcmp(job, "tell") ? "message" : job); return true; }
    snprintf(out, size, "%c\nnot set", 'A' + i);
    return false;
}

static void on_vbutton(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char job[BUTTON_JOB];
    if (!buttons_does(VNAMES[i], PRESS_SHORT, job, sizeof job)) { ui_toast("Settings > Devices > Buttons sets what this one does", 5); return; }
    buttons_tap(VNAMES[i]);            // buttons.c runs what the press is set to, on the next poll
    ui_wake();
}

static void on_actions_gone(lv_event_t *e) { memset(vbtn, 0, sizeof vbtn); }

lv_obj_t *page_actions_build(lv_obj_t *parent)
{
    lv_obj_t *c = column(parent);
    lv_obj_t *g = grid(c);
    int cols = board_w() >= 300 ? 3 : 2, bw = (board_w() - 16 - 6 * (cols - 1)) / cols;   // three a row on a wide glass, two on a narrow
    for (int i = 0; i < VBUTTONS; i++) {
        char words[VLABEL_MAX + 24];
        bool set = vbutton_words(i, words, sizeof words);
        vbtn[i] = button(g, words, on_vbutton, (void *)(intptr_t)i, bw, 58);
        lv_obj_t *l = lv_obj_get_child(vbtn[i], 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, bw - 8);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        if (!set) lv_obj_set_style_text_color(l, C_DIM, 0);
    }
    lv_obj_add_event_cb(c, on_actions_gone, LV_EVENT_DELETE, NULL);
    return c;
}

void page_actions_label(int which, const char *words)
{
    if (which < 0 || which >= VBUTTONS) return;
    ui_copy_text(vlabels[which], words, sizeof vlabels[which]);
    if (!vbtn[which]) return;
    char text_[VLABEL_MAX + 24];
    bool set = vbutton_words(which, text_, sizeof text_);
    lv_obj_t *l = lv_obj_get_child(vbtn[which], 0);
    lv_label_set_text(l, text_);
    lv_obj_set_style_text_color(l, set ? C_TEXT : C_DIM, 0);
}
