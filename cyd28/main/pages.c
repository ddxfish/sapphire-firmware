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
             board()->cal_set ? "" : ", touch NOT calibrated");
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

static lv_obj_t *bk_head, *bk_list, *bk_bar, *bk_prog, *bk_btn;
static lv_timer_t *bk_timer;
static int bk_count = -1;
static bool bk_writing;

// "sapphire_2026-10-15_214832_manual.sapphirebak" -> "2026-10-15 21:48  manual"
static void backup_words(const char *name, char *out, size_t size)
{
    const char *p = strncmp(name, "sapphire_", 9) ? name : name + 9;
    char date[11] = {0}, clock[7] = {0}, kind[24] = {0};
    if (strlen(p) > 18 && p[10] == '_' && sscanf(p, "%10[0-9-]_%6[0-9]_%23[^.]", date, clock, kind) >= 2)
        snprintf(out, size, "%s %.2s:%.2s  %s", date, clock, clock + 2, kind);
    else snprintf(out, size, "%.40s", name);
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
    char all[720] = "";
    for (int i = 0; i < n; i++) {
        char words[80], one[128];
        backup_words(rows[i].name, words, sizeof words);
        snprintf(one, sizeof one, "%s%s   %llu MB", i ? "\n" : "", words, (unsigned long long)(rows[i].size >> 20));
        strlcat(all, one, sizeof all);
    }
    lv_label_set_text(bk_list, n ? all : (storage_ok() ? "No backups on the card yet." : ""));
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
        lv_obj_add_state(bk_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_flag(bk_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bk_prog, LV_OBJ_FLAG_HIDDEN);
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
    link_ask("backup");                // Sapphire answers with a line; link.c shows it
    lv_obj_add_state(bk_btn, LV_STATE_DISABLED);
}

static void on_backups_gone(lv_event_t *e)
{
    if (bk_timer) lv_timer_delete(bk_timer);
    bk_timer = NULL;
    bk_head = bk_list = bk_bar = bk_prog = bk_btn = NULL;
    bk_count = -1;
}

lv_obj_t *page_backups_build(lv_obj_t *parent)
{
    lv_obj_t *c = column(parent);
    bk_head = text(c, &lv_font_montserrat_16, C_TEXT);
    bk_list = text(c, &lv_font_montserrat_14, C_DIM);
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

static lv_obj_t *bd_note, *bd_torch;

static void board_note(void)
{
    char words[96];
    snprintf(words, sizeof words, "backlight %d%%   flashlight %s   speaker %s", settings.brightness,
             light_torch_on() ? "on" : "off", speaker_ok() ? "ready" : "none");
    lv_label_set_text(bd_note, words);
    lv_label_set_text(lv_obj_get_child(bd_torch, 0), light_torch_on() ? "Flashlight off" : "Flashlight");
}

static void on_torch(lv_event_t *e) { light_torch(!light_torch_on()); board_note(); }

static void on_bright(lv_event_t *e)
{
    int by = (int)(intptr_t)lv_event_get_user_data(e);
    int v = settings.brightness + by;
    settings.brightness = v < 10 ? 10 : v > 100 ? 100 : v;
    settings_save();
    board_backlight(settings.brightness);
    board_note();
}

static void on_tone(lv_event_t *e)
{
    if (!speaker_ok()) { ui_toast("no speaker on this board", 4); return; }
    speaker_tone(880, 350);
}

static void on_dark(lv_event_t *e) { ui_dark(); }

static void on_reload(lv_event_t *e)
{
    ui_show_page(PAGE_CHAT);
    link_ask_history(-1);
}

static void restart_now(lv_timer_t *t) { esp_restart(); }

static void on_restart(lv_event_t *e)
{
    if (storage_busy()) { ui_toast("a backup is arriving: not now", 4); return; }
    ui_toast("restarting", 3);
    lv_timer_set_repeat_count(lv_timer_create(restart_now, 400, NULL), 1);   // once the toast is drawn (a wait here would hold LVGL)
}

static void on_board_gone(lv_event_t *e) { bd_note = bd_torch = NULL; }

lv_obj_t *page_board_build(lv_obj_t *parent)
{
    lv_obj_t *c = column(parent);
    bd_note = text(c, &lv_font_montserrat_14, C_DIM);
    lv_obj_t *g = grid(c);
    bd_torch = button(g, "Flashlight", on_torch, NULL, 96, 44);
    button(g, "Dimmer", on_bright, (void *)(intptr_t)-10, 96, 44);
    button(g, "Brighter", on_bright, (void *)(intptr_t)10, 96, 44);
    button(g, "Tone", on_tone, NULL, 96, 44);
    button(g, "Screen off", on_dark, NULL, 96, 44);
    button(g, "Reload chat", on_reload, NULL, 96, 44);
    button(g, "Restart", on_restart, NULL, 96, 44);
    board_note();
    lv_obj_add_event_cb(c, on_board_gone, LV_EVENT_DELETE, NULL);
    return c;
}

// --- actions: six buttons of your own -----------------------------------------------------------------

static const char *VNAMES[VBUTTONS] = {"a", "b", "c", "d", "e", "f", "g", "h", "i"};
static char vlabels[VBUTTONS][VLABEL_MAX + 1];
static lv_obj_t *vbtn[VBUTTONS];

static void vbutton_words(int i, char *out, size_t size)
{
    if (vlabels[i][0]) snprintf(out, size, "%s", vlabels[i]);
    else snprintf(out, size, "%c\nnot set", 'A' + i);
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
    for (int i = 0; i < VBUTTONS; i++) {
        char words[VLABEL_MAX + 24];
        vbutton_words(i, words, sizeof words);
        vbtn[i] = button(g, words, on_vbutton, (void *)(intptr_t)i, 96, 58);   // three rows of three in the 212 px under the bar
        lv_obj_t *l = lv_obj_get_child(vbtn[i], 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, 88);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        if (!vlabels[i][0]) lv_obj_set_style_text_color(l, C_DIM, 0);
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
    vbutton_words(which, text_, sizeof text_);
    lv_obj_t *l = lv_obj_get_child(vbtn[which], 0);
    lv_label_set_text(l, text_);
    lv_obj_set_style_text_color(l, vlabels[which][0] ? C_TEXT : C_DIM, 0);
}
