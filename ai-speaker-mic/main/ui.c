// ui.c - see ui.h. LVGL 9 on esp_lvgl_port: its own task on core 1 (stack in
// PSRAM, it never writes flash) and a lock for everyone else. LVGL's heap is
// PSRAM too (lv_mem_psram.c); only the two draw buffers are internal DMA RAM.
#include "ui.h"
#include "buttons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "audio.h"
#include "board.h"
#include "lastwords.h"
#include "light.h"
#include "link.h"
#include "net.h"
#include "server.h"
#include "settings.h"
#include "wake.h"

static const char *TAG = "ui";
#define ZONE 72                        // a corner's touch zone, px
#define MIDDLE 120
#define HEARD_S 8
#define PICTURE_MAX_S 3600

static lv_display_t *disp;
static lv_indev_t *indev;
static bool ok;
static lv_obj_t *scene, *picture, *catcher, *chat_lbl, *clock_lbl, *heard, *toast, *halo, *glow, *state_lbl;
static lv_obj_t *zone[4], *zone_btn[4], *zone_lbl[4];
static lv_obj_t *status_card, *status_text_lbl;
static void status_fill(void);
static lv_timer_t *tick, *heard_timer, *toast_timer, *picture_timer;
static char status_text[48], scene_name[48], chat_name[32], brain_name[48];
static int trim = -1;                  // the chat's trim color as 0xRRGGBB, -1 = none: the middle wears it while she rests
static bool screen_on = true, dimmed, asleep;   // dimmed and asleep come by themselves (settings) and go at a touch or when she stirs; screen_on is the corner button
static char last_cue[16];
static volatile int64_t beat_us;        // the tick's last run: main.c's watch restarts the board when it goes stale

typedef struct { uint8_t *px; int w, h; lv_image_dsc_t dsc; bool filling; } image_t;
static image_t images[2];

#define C_TEXT lv_color_hex(0xe8ecf1)
#define C_DIM lv_color_hex(0x8a93a0)
#define C_OFF lv_color_hex(0x6a7078)
#define C_BTN_ON lv_color_hex(0x2a6fdb)      // a corner that is on: sapphire
#define C_BTN_OFF lv_color_hex(0x16191d)
#define BUTTON 56                      // the round button drawn inside a corner's zone

// --- the images ---------------------------------------------------------------------

static void image_apply(int kind)
{
    image_t *im = &images[kind];
    lv_obj_t *obj = kind == UI_SCENE ? scene : picture;
    im->dsc = (lv_image_dsc_t){0};             // no image cache is built (sdkconfig), so a reused descriptor is safe
    im->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    im->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    im->dsc.header.w = im->w;
    im->dsc.header.h = im->h;
    im->dsc.header.stride = im->w * 2;
    im->dsc.data_size = (uint32_t)im->w * im->h * 2;
    im->dsc.data = im->px;
    lv_image_set_src(obj, &im->dsc);
    lv_obj_set_size(obj, im->w, im->h);
    lv_obj_center(obj);
    lv_obj_set_hidden(obj, false);
}

static void image_free(int kind)
{
    image_t *im = &images[kind];
    lv_obj_t *obj = kind == UI_SCENE ? scene : picture;
    if (obj) { lv_image_set_src(obj, NULL); lv_obj_set_hidden(obj, true); }
    free(im->px);
    im->px = NULL;
    im->filling = false;
}

static void on_picture_end(lv_timer_t *t)
{
    image_free(UI_PICTURE);
    lv_timer_delete(picture_timer);
    picture_timer = NULL;
}

static void picture_end(void)
{
    if (picture_timer) { lv_timer_delete(picture_timer); picture_timer = NULL; }
    image_free(UI_PICTURE);
}

// --- what the zones do ---------------------------------------------------------------

static void wake_screen(void)
{
    if (!screen_on) screen_on = true;
    dimmed = asleep = false;
    board_backlight(settings.brightness);
    lv_display_trigger_activity(disp);
}

// a tap while the glass is dark only brings it back; a tap during a picture ends the picture
static bool tap_taken(void)
{
    if (!screen_on || dimmed || asleep) { wake_screen(); return true; }
    if (images[UI_PICTURE].px && !images[UI_PICTURE].filling) { picture_end(); return true; }
    return false;
}

// A corner is a button like any other (buttons.h): its tap does what Sapphire
// set for it, else its own job. The job runs on the buttons' task.
static const char *CORNER[4] = {"top-left", "top-right", "bottom-left", "bottom-right"};

static void on_zone(lv_event_t *e)
{
    if (tap_taken()) return;
    buttons_tap(CORNER[(int)(intptr_t)lv_event_get_user_data(e)]);
}

static void on_middle(lv_event_t *e)
{
    if (tap_taken()) return;
    if (server_speaking()) { server_stop_speech(); ESP_LOGI(TAG, "tap: stop"); }
    else { wake_trigger(); ESP_LOGI(TAG, "tap: listen"); }
}

static void on_catcher(lv_event_t *e) { tap_taken(); }

// --- the tick: the ring's color, the state, the clock, the backlight ------------------

// Only what changed is touched: every set invalidates, and a redraw ten times a
// second for nothing cost a third of core 1 (2026-10-07).
// Each corner wears the sign of the job its tap does now, lit while that thing is on.
static void draw_zones(void)
{
    static const struct { const char *job, *icon; } SIGNS[] = {
        {"light", LV_SYMBOL_TINT}, {"screen", LV_SYMBOL_POWER}, {"wake", LV_SYMBOL_BELL},
        {"mute", LV_SYMBOL_VOLUME_MAX}, {"listen", LV_SYMBOL_AUDIO}, {"tell", LV_SYMBOL_ENVELOPE},
    };
    static int was[4] = {-1, -1, -1, -1};
    bool on[] = {!light_is_dark(), screen_on, wake_enabled(), !audio_muted(), true, true};
    for (int i = 0; i < 4; i++) {
        char job[BUTTON_JOB];
        int sign = -1;
        if (buttons_does(CORNER[i], PRESS_SHORT, job, sizeof job))
            for (int k = 0; k < (int)(sizeof SIGNS / sizeof SIGNS[0]); k++)
                if (!strcmp(job, SIGNS[k].job)) sign = k;
        bool lit = sign >= 0 && on[sign];
        int now = (sign + 1) * 2 + lit;
        if (was[i] == now) continue;
        was[i] = now;
        lv_label_set_text(zone_lbl[i], sign < 0 ? "" : sign == 3 && !lit ? LV_SYMBOL_MUTE : SIGNS[sign].icon);
        lv_obj_set_style_text_color(zone_lbl[i], lit ? lv_color_white() : C_OFF, 0);
        lv_obj_set_style_bg_color(zone_btn[i], lit ? C_BTN_ON : C_BTN_OFF, 0);
    }
}

static void draw_status(void)
{
    // the gear rides in the clock's own pill, the clock's size: one thing to tap (the status page)
    if (status_text[0]) { lv_label_set_text_fmt(clock_lbl, "%s  " LV_SYMBOL_SETTINGS, status_text); return; }
    time_t now = time(NULL);
    if (now < 1600000000) { lv_label_set_text(clock_lbl, LV_SYMBOL_SETTINGS); return; }
    struct tm t;
    localtime_r(&now, &t);
    char text[8];
    strftime(text, sizeof text, "%H:%M", &t);
    lv_label_set_text_fmt(clock_lbl, "%s  " LV_SYMBOL_SETTINGS, text);
}

static void on_tick(lv_timer_t *t)
{
    beat_us = esp_timer_get_time();
    static uint32_t was = 0xffffffff;
    uint8_t r, g, b;
    light_current(&r, &g, &b);
    const char *cue = light_cue_now();
    bool busy = strcmp(cue, "idle") && strcmp(cue, "shown") && strcmp(cue, "starting");
    // At rest the middle wears the chat's trim color, steady, whatever the ring does (it may be
    // dark, and was black on black); while she listens, thinks or speaks it is the ring's color.
    if (!busy && trim >= 0) { r = trim >> 16; g = trim >> 8 & 255; b = trim & 255; }
    uint32_t now_rgb = (r >> 3) << 16 | (g >> 3) << 8 | (b >> 3);     // 32 steps a channel: a breath repaints a few times, not every tick
    if (now_rgb != was) {
        was = now_rgb;
        lv_obj_set_style_bg_color(halo, lv_color_make(r, g, b), 0);
        lv_obj_set_style_bg_color(glow, lv_color_make(r, g, b), 0);
    }
    if (strcmp(cue, last_cue)) {
        strlcpy(last_cue, cue, sizeof last_cue);
        lv_label_set_text(state_lbl, busy ? cue : "");
        if (busy && screen_on) wake_screen();                 // she stirs: the glass comes back from its glow
    }
    draw_zones();
    static int n;
    if (++n % 10 == 0) draw_status();
    uint32_t alone = lv_display_get_inactive_time(disp);
    if (screen_on && !busy && !asleep && settings.off_after_min && alone > (uint32_t)settings.off_after_min * 60000) {
        asleep = true;
        board_backlight(0);
    } else if (screen_on && !busy && !asleep && !dimmed && settings.dim_after_s && alone > (uint32_t)settings.dim_after_s * 1000) {
        dimmed = true;
        board_backlight(settings.dim_percent);
    }
    if (status_card && n % 10 == 0) status_fill();
}

static void on_heard_end(lv_timer_t *t) { lv_obj_set_hidden(heard, true); lv_timer_delete(heard_timer); heard_timer = NULL; }
static void on_toast_end(lv_timer_t *t) { lv_obj_set_hidden(toast, true); lv_timer_delete(toast_timer); toast_timer = NULL; }

// --- building it ------------------------------------------------------------------------

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *small_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

// --- the status page: what this board is and how it is doing, until a tap ----------------
// Built when asked for and deleted when left: it costs nothing while it is not shown,
// and LVGL's heap is PSRAM.

static void status_fill(void)
{
    if (!status_card) return;
    char text[480], up[24];
    uint32_t s = esp_timer_get_time() / 1000000;
    if (s >= 3600) snprintf(up, sizeof up, "%uh %um", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
    else snprintf(up, sizeof up, "%um %us", (unsigned)(s / 60), (unsigned)(s % 60));
    cJSON *words = lastwords_json(false);
    const cJSON *why = words ? cJSON_GetObjectItem(words, "reason") : NULL;
    snprintf(text, sizeof text,
             "%s\n"
             "chat  %s\n"
             "brain  %s\n"
             "%s   %d dBm\n"
             "Sapphire  %s\n"
             "program  %s   up %s\n"
             "RAM  %u KB free (low %u), %u MB spare\n"
             "started by  %s\n"
             "%s%s",
             settings.name, chat_name[0] ? chat_name : "-", brain_name[0] ? brain_name : "-", net_ip()[0] ? net_ip() : "no WiFi", net_rssi(),
             link_connected() ? "linked" : "NOT linked", PROJECT_VER, up,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024)),
             lastwords_reset(), cJSON_IsString(why) ? "last down  " : "", cJSON_IsString(why) ? why->valuestring : "");
    cJSON_Delete(words);
    lv_label_set_text(status_text_lbl, text);
}

static void on_status_close(lv_event_t *e)
{
    lv_obj_delete(status_card);
    status_card = NULL;
}

static void on_about(lv_event_t *e)
{
    if (tap_taken() || status_card) return;
    status_card = plain(lv_layer_top());
    lv_obj_set_size(status_card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(status_card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(status_card, LV_OPA_90, 0);
    lv_obj_set_clickable(status_card, true);
    lv_obj_add_event_cb(status_card, on_status_close, LV_EVENT_CLICKED, NULL);
    status_text_lbl = small_label(status_card, &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_width(status_text_lbl, BOARD_W - 24);
    lv_label_set_long_mode(status_text_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(status_text_lbl, 5, 0);
    lv_obj_align(status_text_lbl, LV_ALIGN_TOP_LEFT, 12, 10);
    lv_obj_t *hint = small_label(status_card, &lv_font_montserrat_12, C_DIM);
    lv_label_set_text(hint, "tap to close");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    status_fill();
}

static void pill(lv_obj_t *label)
{
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_50, 0);
    lv_obj_set_style_radius(label, 8, 0);
    lv_obj_set_style_pad_hor(label, 8, 0);
    lv_obj_set_style_pad_ver(label, 2, 0);
}

static void build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollable(scr, false);

    scene = lv_image_create(scr);                       // the chat's scene, under everything
    lv_obj_set_hidden(scene, true);
    catcher = plain(scr);                               // a tap anywhere: wake the glass, end a picture
    lv_obj_set_size(catcher, LV_PCT(100), LV_PCT(100));
    lv_obj_set_clickable(catcher, true);
    lv_obj_add_event_cb(catcher, on_catcher, LV_EVENT_CLICKED, NULL);

    // top middle: the clock, her state under it. Bottom middle: the chat this board talks in.
    // Each on a dark pill, so it reads over any scene (laid out 2026-10-09).
    clock_lbl = small_label(scr, &lv_font_montserrat_28, C_TEXT);
    pill(clock_lbl);
    lv_obj_align(clock_lbl, LV_ALIGN_TOP_MID, 0, 6);
    state_lbl = small_label(scr, &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(state_lbl, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_t *about = plain(scr);                       // the clock and its gear: the status page
    lv_obj_set_size(about, BOARD_W - 2 * ZONE - 8, 46);
    lv_obj_align(about, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_clickable(about, true);
    lv_obj_add_event_cb(about, on_about, LV_EVENT_CLICKED, NULL);
    chat_lbl = small_label(scr, &lv_font_montserrat_16, C_TEXT);
    pill(chat_lbl);
    lv_obj_set_style_max_width(chat_lbl, BOARD_W - 2 * ZONE - 8, 0);
    lv_label_set_long_mode(chat_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(chat_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_hidden(chat_lbl, true);

    // the middle: the ring's color as a halo; a tap listens or stops her
    lv_obj_t *mid = plain(scr);
    lv_obj_set_size(mid, MIDDLE, MIDDLE);
    lv_obj_center(mid);
    lv_obj_set_clickable(mid, true);
    lv_obj_add_event_cb(mid, on_middle, LV_EVENT_CLICKED, NULL);
    glow = plain(mid);                                  // two plain circles: a blurred shadow costs a redraw dearly
    lv_obj_set_size(glow, 76, 76);
    lv_obj_center(glow);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_30, 0);
    lv_obj_set_clickable(glow, false);
    halo = plain(mid);
    lv_obj_set_size(halo, 44, 44);
    lv_obj_center(halo);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_COVER, 0);
    lv_obj_set_clickable(halo, false);

    // what she heard, near the bottom
    heard = small_label(scr, &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_width(heard, BOARD_W - 2 * ZONE);
    lv_label_set_long_mode(heard, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(heard, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(heard, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(heard, LV_OPA_50, 0);
    lv_obj_set_style_radius(heard, 6, 0);
    lv_obj_set_style_pad_all(heard, 6, 0);
    lv_obj_align(heard, LV_ALIGN_BOTTOM_MID, 0, -40);          // between the two lower corners, over the chat's name
    lv_obj_set_hidden(heard, true);

    // the four corners
    static const lv_align_t where[4] = {LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT};
    for (int i = 0; i < 4; i++) {
        zone[i] = plain(scr);
        lv_obj_set_size(zone[i], ZONE, ZONE);
        lv_obj_align(zone[i], where[i], 0, 0);
        lv_obj_set_clickable(zone[i], true);
        lv_obj_add_event_cb(zone[i], on_zone, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        zone_btn[i] = plain(zone[i]);                   // a round button: sapphire when on, dark when off
        lv_obj_set_size(zone_btn[i], BUTTON, BUTTON);
        lv_obj_align(zone_btn[i], where[i], (i % 2) ? -6 : 6, (i < 2) ? 6 : -6);
        lv_obj_set_style_radius(zone_btn[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(zone_btn[i], LV_OPA_80, 0);
        lv_obj_set_style_border_color(zone_btn[i], lv_color_hex(0x3a4048), 0);
        lv_obj_set_style_border_width(zone_btn[i], 1, 0);
        lv_obj_set_clickable(zone_btn[i], false);       // the whole zone takes the tap
        zone_lbl[i] = small_label(zone_btn[i], &lv_font_montserrat_28, C_DIM);
        lv_obj_center(zone_lbl[i]);
        lv_obj_set_clickable(zone_lbl[i], false);
    }

    // her picture, over everything but the toast
    picture = lv_image_create(scr);
    lv_obj_set_hidden(picture, true);
    lv_obj_set_clickable(picture, true);
    lv_obj_add_event_cb(picture, on_catcher, LV_EVENT_CLICKED, NULL);

    // her line across the top
    toast = lv_label_create(lv_layer_top());
    lv_obj_set_width(toast, LV_PCT(100));
    lv_label_set_long_mode(toast, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(toast, &lv_font_montserrat_16, 0);
    lv_obj_set_style_bg_color(toast, lv_palette_darken(LV_PALETTE_BLUE, 3), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(toast, 8, 0);
    lv_obj_set_style_text_color(toast, C_TEXT, 0);
    lv_obj_set_style_text_align(toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_hidden(toast, true);

    draw_zones();
    draw_status();
    tick = lv_timer_create(on_tick, 100, NULL);
}

static void on_touch_read(lv_indev_t *dev, lv_indev_data_t *data)
{
    int x, y;
    if (board_touch_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else data->state = LV_INDEV_STATE_RELEASED;
}

void ui_start(void)
{
    board_glass_t *g = board_glass();
    if (!g->up) { ESP_LOGW(TAG, "no glass"); return; }
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_priority = 4;                            // under the ring (6) and the server (5)
    port.task_stack = 8192;
    port.task_affinity = 1;                            // core 0 is WiFi + the AFE's feed
    port.task_stack_caps = MALLOC_CAP_SPIRAM;          // nothing on this task writes flash
    port.task_max_sleep_ms = 500;
    port.timer_period_ms = 5;
    if (lvgl_port_init(&port) != ESP_OK) { ESP_LOGE(TAG, "LVGL did not start"); return; }
    const lvgl_port_display_cfg_t dcfg = {
        .io_handle = g->io, .panel_handle = g->panel,
        .buffer_size = BOARD_W * BOARD_DRAW_LINES, .double_buffer = true,
        .hres = BOARD_W, .vres = BOARD_H, .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = true, .mirror_x = !settings.flip, .mirror_y = settings.flip},   // landscape, as board_glass_orient
        .flags = {.buff_dma = true, .buff_spiram = true, .swap_bytes = true},   // in PSRAM, DMA reads it there (board.c psram_dma_direct)
    };
    disp = lvgl_port_add_disp(&dcfg);
    if (!disp) { ESP_LOGE(TAG, "LVGL did not take the panel"); return; }
    lvgl_port_lock(0);
    if (g->touch) {                                    // our own reader (board_touch_read), polled by LVGL every 33 ms
        indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, on_touch_read);
        lv_indev_set_display(indev, disp);
    }
    build();
    beat_us = esp_timer_get_time();
    lvgl_port_unlock();
    ok = true;
    board_backlight(settings.brightness);
    ESP_LOGI(TAG, "glass built, %s", indev ? "touch on" : "NO touch");
}

bool ui_ok(void) { return ok; }

// --- from other tasks ------------------------------------------------------------------
// A door waits this long for LVGL's lock and then gives up: a glass that is
// stuck must not take the web server down with it (2026-10-08: one `show`
// waited forever and the board went offline).
#define DOOR_MS 500
static int doors_shut;

static bool take(void)
{
    if (lvgl_port_lock(DOOR_MS)) return true;
    doors_shut++;
    return false;
}

int ui_beat_age_ms(void) { return ok ? (int)((esp_timer_get_time() - beat_us) / 1000) : -1; }
int ui_doors_shut(void) { return doors_shut; }

bool ui_toast(const char *text, int seconds)
{
    if (!ok || !take()) return false;
    if (toast_timer) { lv_timer_delete(toast_timer); toast_timer = NULL; }
    if (!text || !text[0]) lv_obj_set_hidden(toast, true);
    else {
        char *copy = strndup(text, 300);
        if (copy) { lv_label_set_text(toast, copy); free(copy); }
        lv_obj_set_hidden(toast, false);
        toast_timer = lv_timer_create(on_toast_end, (seconds > 0 ? seconds : 20) * 1000, NULL);
        lv_timer_set_repeat_count(toast_timer, 1);
        if (screen_on) wake_screen();
    }
    lvgl_port_unlock();
    return true;
}

bool ui_chat(const char *name, const char *brain, int trim_rgb)
{
    if (!ok || !take()) return false;
    strlcpy(chat_name, name ? name : "", sizeof chat_name);
    strlcpy(brain_name, brain ? brain : "", sizeof brain_name);
    trim = trim_rgb;
    lv_label_set_text(chat_lbl, chat_name);
    lv_obj_set_hidden(chat_lbl, !chat_name[0]);
    lvgl_port_unlock();
    return true;
}

const char *ui_chat_name(void) { return chat_name; }
const char *ui_brain_name(void) { return brain_name; }
int ui_trim(void) { return trim; }

void ui_heard(const char *text)
{
    if (!ok || !text || !text[0] || !take()) return;
    char line[200];
    snprintf(line, sizeof line, "\"%s\"", text);
    lv_label_set_text(heard, line);
    lv_obj_set_hidden(heard, false);
    if (heard_timer) lv_timer_delete(heard_timer);
    heard_timer = lv_timer_create(on_heard_end, HEARD_S * 1000, NULL);
    lv_timer_set_repeat_count(heard_timer, 1);
    lvgl_port_unlock();
}

void ui_status(const char *text)
{
    if (!ok) return;
    strlcpy(status_text, text ? text : "", sizeof status_text);
    if (!take()) return;
    draw_status();
    lvgl_port_unlock();
}

uint8_t *ui_image_begin(int kind, int w, int h)
{
    if (!ok || kind < 0 || kind > 1 || w < 1 || h < 1 || w > BOARD_W || h > BOARD_H || !take()) return NULL;
    image_t *im = &images[kind];
    uint8_t *px = NULL;
    if (!im->filling) {
        uint8_t *fresh = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
        if (fresh) {
            if (kind == UI_PICTURE && picture_timer) { lv_timer_delete(picture_timer); picture_timer = NULL; }
            image_free(kind);                             // the old one goes only once the new one has memory
            im->px = px = fresh;
            im->w = w; im->h = h;
            im->filling = true;
        }
    }
    lvgl_port_unlock();
    return px;
}

bool ui_image_show(int kind, int w, int h, int seconds, const char *name)
{
    if (!ok || kind < 0 || kind > 1 || !take()) return false;
    image_t *im = &images[kind];
    if (im->px && im->filling && im->w == w && im->h == h) {
        uint16_t *p = (uint16_t *)im->px;                // the wire is big-endian, LVGL's RGB565 is little
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) p[i] = (uint16_t)(p[i] << 8 | p[i] >> 8);
        im->filling = false;
        image_apply(kind);
        if (kind == UI_SCENE) strlcpy(scene_name, name ? name : "", sizeof scene_name);
        else {
            if (seconds < 1) seconds = 60;
            if (seconds > PICTURE_MAX_S) seconds = PICTURE_MAX_S;
            picture_timer = lv_timer_create(on_picture_end, seconds * 1000, NULL);
            lv_timer_set_repeat_count(picture_timer, 1);
            if (screen_on) wake_screen();
        }
    }
    lvgl_port_unlock();
    return true;
}

bool ui_image_drop(int kind)
{
    if (!ok || kind < 0 || kind > 1 || !take()) return false;
    if (kind == UI_PICTURE) picture_end();
    else { image_free(UI_SCENE); scene_name[0] = 0; }
    lvgl_port_unlock();
    return true;
}

const char *ui_scene_name(void) { return scene_name; }
bool ui_picture_on(void) { return images[UI_PICTURE].px && !images[UI_PICTURE].filling; }

bool ui_screen_power(bool on)
{
    if (!ok || !take()) return false;
    if (on) wake_screen();
    else { screen_on = false; board_backlight(0); }
    lvgl_port_unlock();
    return true;
}

bool ui_screen_on(void) { return screen_on; }

// After the screen's settings change: shown at once, as if it had just been touched,
// and painted afresh (a turned panel keeps its old picture until then).
bool ui_relight(void)
{
    if (!ok || !take()) return false;
    lv_obj_invalidate(lv_screen_active());
    if (screen_on) wake_screen();
    lvgl_port_unlock();
    return true;
}

// --- the test card ------------------------------------------------------------------------

static void on_card_touch(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    printf("TOUCH %d %d\n", (int)p.x, (int)p.y);
    fflush(stdout);
}

static void on_card_back(lv_event_t *e) { lv_obj_delete(lv_event_get_user_data(e)); }

void ui_test_card(void)
{
    if (!ok || !take()) return;
    wake_screen();
    lv_obj_t *card = plain(lv_layer_top());
    lv_obj_set_size(card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_clickable(card, true);
    lv_obj_add_event_cb(card, on_card_touch, LV_EVENT_PRESSED, NULL);
    static const struct { const char *name; uint32_t hex; } bars[] = {{"RED", 0xff0000}, {"GREEN", 0x00ff00}, {"BLUE", 0x0000ff}, {"WHITE", 0xffffff}};
    int step = (BOARD_W - 16) / 4, wide = step - 8, tall = BOARD_H / 3;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *bar = plain(card);
        lv_obj_set_size(bar, wide, tall);
        lv_obj_set_style_bg_color(bar, lv_color_hex(bars[i].hex), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_pos(bar, 8 + i * step, BOARD_H / 4);
        lv_obj_t *l = small_label(card, &lv_font_montserrat_12, lv_color_white());
        lv_label_set_text(l, bars[i].name);
        lv_obj_set_pos(l, 8 + i * step, BOARD_H / 4 + tall + 6);
    }
    lv_obj_t *t = small_label(card, &lv_font_montserrat_20, lv_color_white());
    lv_label_set_text(t, "TOP");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_t *hint = small_label(card, &lv_font_montserrat_12, lv_color_white());
    lv_label_set_text(hint, "tap anywhere: the console\nsays where. `screen flip 1` turns it.");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_size(btn, 120, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_event_cb(btn, on_card_back, LV_EVENT_CLICKED, card);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "done");
    lv_obj_center(bl);
    lvgl_port_unlock();
}

// The glass's lock taken and never given back, from the caller's task: what a stuck glass looks like to everyone else.
void ui_test_stuck(void)
{
    if (!ok) return;
    lvgl_port_lock(0);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
