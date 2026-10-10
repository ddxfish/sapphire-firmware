// ui.c - see ui.h. LVGL 9 through esp_lvgl_port, which runs LVGL on its own
// task and hands out the lock.
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "board.h"
#include "buttons.h"
#include "net.h"
#include "settings.h"
#include "status.h"

static const char *TAG = "ui";
#define SLEEP_AFTER_S 120

static lv_display_t *disp;
static lv_obj_t *line1, *line2, *toast, *card;
static lv_timer_t *toast_timer, *card_timer;
static bool dark;
static int64_t awake_since_ms;
static status_t last = -1;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void room_text(const storage_state_t *c, char *out, size_t size)
{
    uint64_t free_b = 0, total = 0;
    if (!storage_room(&free_b, &total) || !total) { strlcpy(out, "card?", size); return; }
    double used = (total - free_b) / 1073741824.0, all = total / 1073741824.0;
    if (all < 1) snprintf(out, size, "%d/%d MB", (int)((total - free_b) >> 20), (int)(total >> 20));
    else if (used < 10) snprintf(out, size, "%.1f/%.0f GB", used, all);
    else snprintf(out, size, "%.0f/%.0f GB", used, all);
}

// "14  21:40" when the newest is from today, "14  Oct 9" when it is older
static void newest_text(const storage_state_t *c, char *out, size_t size)
{
    static const char *MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t made = status_newest_at(c->newest), now = time(NULL);
    struct tm m, n;
    if (!made) { snprintf(out, size, "%d kept", c->count); return; }
    localtime_r(&made, &m);
    localtime_r(&now, &n);
    if (m.tm_year == n.tm_year && m.tm_yday == n.tm_yday) snprintf(out, size, "%d  %02d:%02d", c->count, m.tm_hour, m.tm_min);
    else snprintf(out, size, "%d  %s %d", c->count, MONTHS[m.tm_mon], m.tm_mday);
}

static void draw(void)
{
    storage_state_t c;
    status_t st = status_now(&c);
    char a[24] = "", b[24] = "";
    switch (st) {
    case ST_SETUP: strcpy(a, "NOT SET UP"); strcpy(b, "see Sapphire"); break;
    case ST_WRITING:
        snprintf(a, sizeof a, "WRITE %d%%", c.expect ? (int)(c.got * 100 / c.expect) : 0);
        strcpy(b, "do not pull");
        break;
    case ST_NOCARD: strcpy(a, "NO CARD"); strcpy(b, "FAT32 only"); break;
    case ST_DAMAGED: snprintf(a, sizeof a, "%d DAMAGED", c.damaged); room_text(&c, b, sizeof b); break;
    case ST_CHECKING: strcpy(a, "CHECKING"); snprintf(b, sizeof b, "%d of %d", c.checked, c.to_check); break;
    case ST_FAILED: strcpy(a, "LAST FAILED"); room_text(&c, b, sizeof b); break;
    case ST_EMPTY: room_text(&c, a, sizeof a); strcpy(b, "no backups"); break;
    case ST_OFFLINE: room_text(&c, a, sizeof a); strcpy(b, net_up() ? "no Sapphire" : "no WiFi"); break;
    default: room_text(&c, a, sizeof a); newest_text(&c, b, sizeof b); break;
    }
    const look_t *l = status_look(st);
    lv_color_t color = lv_color_make(l->r, l->g > 60 || l->r || l->b > 200 ? l->g : 120, l->b);   // the LED's blue is too dark to read
    lv_label_set_text(line1, a);
    lv_label_set_text(line2, b);
    lv_obj_set_style_text_color(line1, color, 0);
    lv_obj_set_style_text_color(line2, st == ST_GOOD ? lv_color_hex(0xb0b8c0) : color, 0);
    if (st != last) {                        // something changed: show it
        last = st;
        if (dark) { dark = false; board_backlight(settings.brightness); }
        awake_since_ms = now_ms();
    }
    if (st == ST_WRITING || st == ST_CHECKING) awake_since_ms = now_ms();
    if (!dark && now_ms() - awake_since_ms > SLEEP_AFTER_S * 1000) { dark = true; board_backlight(0); }
}

static void on_tick(lv_timer_t *t) { draw(); buttons_poll(); }
static void on_fast(lv_timer_t *t) { buttons_poll(); }

static void on_toast_end(lv_timer_t *t)
{
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(toast_timer);
    toast_timer = NULL;
}

static void on_card_end(lv_timer_t *t)
{
    lv_obj_delete(card);
    card = NULL;
    lv_timer_delete(card_timer);
    card_timer = NULL;
}

static lv_obj_t *big(lv_obj_t *on, int y)
{
    lv_obj_t *l = lv_label_create(on);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, BOARD_W);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
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
        .rotation = {.swap_xy = true, .mirror_x = !b->flip, .mirror_y = b->flip},
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    disp = lvgl_port_add_disp(&dcfg);
    if (!disp) { ESP_LOGE(TAG, "LVGL did not take the screen"); return; }
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    line1 = big(scr, 9);
    line2 = big(scr, 44);
    toast = lv_label_create(lv_layer_top());
    lv_obj_set_size(toast, BOARD_W, BOARD_H);
    lv_obj_set_style_bg_color(toast, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(toast, 4, 0);
    lv_obj_set_style_text_font(toast, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(toast, lv_color_hex(0xe0e6ee), 0);
    lv_label_set_long_mode(toast, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    draw();
    lv_timer_create(on_tick, 500, NULL);
    lv_timer_create(on_fast, 40, NULL);      // the button's jobs run here (buttons.c reads the pin itself)
    lvgl_port_unlock();
    awake_since_ms = now_ms();
    board_backlight(settings.brightness);
    ESP_LOGI(TAG, "screen up");
}

void ui_toast(const char *text, int seconds)
{
    if (!disp) return;
    lvgl_port_lock(0);
    if (toast_timer) { lv_timer_delete(toast_timer); toast_timer = NULL; }
    if (text && text[0]) {
        lv_label_set_text(toast, text);
        lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
        toast_timer = lv_timer_create(on_toast_end, (seconds > 0 ? seconds : 8) * 1000, NULL);
    } else lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
    ui_wake();
}

bool ui_wake(void)
{
    bool was = dark;
    dark = false;
    awake_since_ms = now_ms();
    board_backlight(settings.brightness);
    return was;
}

void ui_sleep(void)
{
    dark = true;
    board_backlight(0);
}

void ui_test_card(void)
{
    if (!disp) return;
    static const uint32_t BARS[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff};
    lvgl_port_lock(0);
    if (!card) {
        card = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, BOARD_W, BOARD_H);
        lv_obj_set_style_bg_color(card, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        for (int i = 0; i < 4; i++) {
            lv_obj_t *bar = lv_obj_create(card);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, BOARD_W / 4, BOARD_H / 2);
            lv_obj_set_pos(bar, i * BOARD_W / 4, BOARD_H / 2);
            lv_obj_set_style_bg_color(bar, lv_color_hex(BARS[i]), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        }
        lv_obj_t *l = lv_label_create(card);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_label_set_text(l, "TOP  red green blue white");
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 2, 2);
        lv_obj_t *frame = lv_obj_create(card);
        lv_obj_remove_style_all(frame);
        lv_obj_set_size(frame, BOARD_W, BOARD_H);
        lv_obj_set_style_border_color(frame, lv_color_hex(0xffff00), 0);
        lv_obj_set_style_border_width(frame, 1, 0);
        card_timer = lv_timer_create(on_card_end, 20000, NULL);
    }
    lvgl_port_unlock();
    ui_wake();
}
