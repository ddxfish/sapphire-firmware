// ui.c - see ui.h. LVGL 9 on esp_lvgl_port: its own task on core 1 (stack in
// PSRAM, it never writes flash) and a lock for everyone else. LVGL's heap is
// PSRAM too (lv_mem_psram.c), and so are the two whole-frame draw buffers (the
// panel's DMA reads them there): Elecrow's recipe for a glass that feels quick.
#include "ui.h"
#include "buttons.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "audio.h"
#include "board.h"
#include "camera.h"
#include "lastwords.h"
#include "light.h"
#include "link.h"
#include "net.h"
#include "power.h"
#include "server.h"
#include "settings.h"
#include "avatar.h"
#include "storage.h"
#include "wake.h"

static const char *TAG = "ui";
#define ZONE 72
#define BAND 72
#define HEARD_S 8
#define PICTURE_MAX_S 3600
#define HOLD_MS 1200
#define FACE 240                       // the square a face stream paints: the glass's narrow side
#define SHORT_GLASS (BOARD_H < 260)    // the speaker's 240-tall landscape glass: the pendant's 284 layouts ran off its bottom
#define FACE_TAP 100                   // the centre square of her face that is the talk tap in avatar mode (the halo's 64 px + its 30 px reach, about)

static lv_display_t *disp;
static lv_indev_t *indev;
static bool ok;
static lv_obj_t *tv, *app[APPS], *page[APPS], *page_lbl[APPS];
// VOICE
static lv_obj_t *scene, *halo, *glow, *clock_lbl, *state_lbl, *chat_lbl, *heard, *wake_btn, *mute_btn, *wake_lbl, *mute_lbl, *light_btn, *screen_btn, *light_lbl, *screen_lbl;
static lv_obj_t *dial_card, *dial_lbl;
static int dial_kind;
// FACE
static lv_obj_t *face;                 // the picture door's stream (a game's table) on her tile
static lv_obj_t *av_img;               // her stream (avatar.c) on her tile
static uint8_t *av_px;                 // its face, BOARD_AV_W x BOARD_AV_H RGB565 big-endian, PSRAM
static int dirty_x1, dirty_y1, dirty_x2, dirty_y2;   // regions written into av_px but not yet on the panel (a late one, or the lock was busy)
static lv_image_dsc_t av_dsc;
static lv_timer_t *av_hide;
static volatile bool healing;           // ui_heal pulses the touch chip's reset: the touch task waits
static volatile bool labels_due;        // Actions labels arrived while the glass was busy
static volatile bool av_shown;          // on_tick's last av_shown_now(): what the HTTP tasks read (ui_av_shown)
static bool av_shown_now(void);
static bool top_shown(void)             // anything visible on the top layer (a toast, a dial, a picture): her blit would go under it
{
    lv_obj_t *top = lv_layer_top();
    for (uint32_t i = 0, n = lv_obj_get_child_count(top); i < n; i++) if (!lv_obj_is_hidden(lv_obj_get_child(top, i))) return true;
    return false;
}
// SETTINGS
static lv_obj_t *bright_sl, *vol_sl, *fill_sw, *aec_sw;
// BACKUPS
static lv_obj_t *bk_head, *bk_rows, *bk_bar, *bk_prog, *bk_btn, *bk_check;
#define BK_ROWS_MAX 6
static int bk_shown = BK_ROWS_MAX;     // the newest backups listed: as many as the glass holds above its buttons
static lv_obj_t *vbtn[VBUTTONS];       // the Actions tile's nine
static char vlabels[VBUTTONS][VLABEL_MAX + 1];
static const char *VNAMES[VBUTTONS] = {"a", "b", "c", "d", "e", "f", "g", "h", "i"};
static int bk_count = -1;
static bool bk_writing, bk_checking;
// over everything
static lv_obj_t *picture, *shade, *toast, *picker, *picker_scrim;

// Seven places tile the glass by name: a game's taps (?taps=1) are named by them, and the
// halo and the two round buttons are buttons by these names (main.c BUTTONS).
#define ZONES_N 7
static const ui_zone_t ZONES[ZONES_N] = {
    {"top-left", 0, 0, ZONE, BOARD_H / 2},
    {"top-right", BOARD_W - ZONE, 0, ZONE, BOARD_H / 2},
    {"bottom-left", 0, BOARD_H / 2, ZONE, BOARD_H / 2},
    {"bottom-right", BOARD_W - ZONE, BOARD_H / 2, ZONE, BOARD_H / 2},
    {"top", ZONE, 0, BOARD_W - 2 * ZONE, BAND},
    {"bottom", ZONE, BOARD_H - BAND, BOARD_W - 2 * ZONE, BAND},
    {"middle", ZONE, BAND, BOARD_W - 2 * ZONE, BOARD_H - 2 * BAND},
};

static bool stream_taps;
#define STREAM_LEFT_S 75
static int64_t stream_left_until_us;
static bool take(void);
static lv_obj_t *plain(lv_obj_t *parent);
static lv_obj_t *small_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color);
static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_align_t where, int dx, int dy, lv_event_cb_t cb);
static void backups_fill(void);
static lv_timer_t *tick, *heard_timer, *toast_timer, *picture_timer;
static char status_text[48], scene_name[64], chat_name[32], brain_name[48];
static int trim = -1;
static bool screen_on = true, dimmed, asleep, sleeping;   // sleeping: power.c holds the lock; asleep: the backlight timer put it out
static bool swallow;                   // a touch that only woke the glass: nothing acts on it until the finger lifts
static char last_cue[16];
static void ear_set(bool on);           // the listen bar in her face (below)
static volatile int64_t beat_us;

typedef struct { uint8_t *px, *old; int w, h; lv_image_dsc_t dsc; bool filling; } image_t;
static image_t images[2];

static uint8_t *next_px;
static int next_w, next_h;
static lv_timer_t *frame_timer;
static int64_t frame_painted_us;
static int frames_painted, frames_replaced;

static void stream_stop(void)
{
    if (tv) lv_obj_set_scrollable(tv, true);   // whatever ended the stream, swipes move between tiles again
    stream_taps = false;
    free(next_px);
    next_px = NULL;
    if (frame_timer) { lv_timer_delete(frame_timer); frame_timer = NULL; }
}

#define C_TEXT lv_color_hex(0xe8ecf1)
#define C_DIM lv_color_hex(0x8a93a0)
#define C_OFF lv_color_hex(0x6a7078)
#define C_ON lv_color_hex(0x2a6fdb)
#define C_BTN_OFF lv_color_hex(0x16191d)
#define C_LINE lv_color_hex(0x3a4048)
#define BUTTON 48

static bool is_stream(const image_t *im) { return im->dsc.header.cf == LV_COLOR_FORMAT_RGB565_SWAPPED; }
int ui_app(void)
{
    if (!tv) return APP_VOICE;
    lv_obj_t *t = lv_tileview_get_tile_active(tv);
    for (int i = 0; i < APPS; i++) if (t == app[i] || t == page[i]) return i;
    return APP_VOICE;
}
static bool on_avatar_tile(void) { return tv && lv_tileview_get_tile_active(tv) == app[APP_AVATAR] && !lv_obj_is_scrolling(tv) && !lv_anim_get(tv, NULL); }   // settled: the snap after a release moves the tile too

// --- the images ---------------------------------------------------------------------

static void image_apply(int kind, lv_color_format_t cf)
{
    image_t *im = &images[kind];
    lv_obj_t *obj = kind == UI_SCENE ? scene : cf == LV_COLOR_FORMAT_RGB565_SWAPPED ? face : picture;
    im->dsc = (lv_image_dsc_t){0};
    im->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    im->dsc.header.cf = cf;
    im->dsc.header.w = im->w;
    im->dsc.header.h = im->h;
    im->dsc.header.stride = im->w * 2;
    im->dsc.data_size = (uint32_t)im->w * im->h * 2;
    im->dsc.data = im->px;
    lv_image_set_src(obj, &im->dsc);
    lv_obj_set_size(obj, im->w, im->h);
    lv_obj_center(obj);
    if (obj == picture) {
        lv_obj_set_style_bg_color(shade, trim >= 0 ? lv_color_mix(lv_color_hex(trim), lv_color_black(), 40) : lv_color_black(), 0);
        lv_obj_set_hidden(shade, false);
        lv_obj_set_hidden(face, true);
    } else if (obj == face) {
        lv_obj_set_hidden(picture, true);
        lv_obj_set_hidden(shade, true);
    }
    lv_obj_set_hidden(obj, false);
}

static void image_free(int kind)
{
    image_t *im = &images[kind];
    if (kind == UI_SCENE) { if (scene) { lv_image_set_src(scene, NULL); lv_obj_set_hidden(scene, true); } }
    else if (picture) {
        lv_image_set_src(picture, NULL); lv_obj_set_hidden(picture, true);
        lv_image_set_src(face, NULL); lv_obj_set_hidden(face, true);
        lv_obj_set_hidden(shade, true);
    }
    free(im->px);
    free(im->old);
    im->px = im->old = NULL;
    im->filling = false;
}

static void picture_drop(void)
{
    image_t *im = &images[UI_PICTURE];
    if (im->filling) {
        lv_image_set_src(picture, NULL); lv_obj_set_hidden(picture, true);
        lv_image_set_src(face, NULL); lv_obj_set_hidden(face, true);
        lv_obj_set_hidden(shade, true);
        free(im->old);
        im->old = NULL;
    } else image_free(UI_PICTURE);
}

static void on_picture_end(lv_timer_t *t)
{
    stream_stop();
    picture_drop();
    lv_timer_delete(picture_timer);
    picture_timer = NULL;
}

static void picture_timer_arm(int seconds)
{
    if (picture_timer) lv_timer_delete(picture_timer);
    if (seconds < 1) seconds = 60;
    if (seconds > PICTURE_MAX_S) seconds = PICTURE_MAX_S;
    picture_timer = lv_timer_create(on_picture_end, seconds * 1000, NULL);
    lv_timer_set_repeat_count(picture_timer, 1);
}

static void picture_end(void)
{
    if (picture_timer) { lv_timer_delete(picture_timer); picture_timer = NULL; }
    stream_stop();
    picture_drop();
}

// --- waking the glass, taps -----------------------------------------------------------

static void wake_screen(void)
{
    if (!screen_on) screen_on = true;
    dimmed = asleep = false;
    if (!sleeping) board_backlight(settings.brightness);
    lv_display_trigger_activity(disp);
}

#define STREAM_HIDE_S 10
static bool stream_hidden;
static int64_t stream_hide_until_us;

// A tap on a still picture ends it; on a stream it hides the stream while she talks.
static void on_picture_tap(lv_event_t *e)
{
    if (stream_taps && images[UI_PICTURE].px && !images[UI_PICTURE].filling) {   // a game owns the taps: the place by name
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        for (int i = 0; i < ZONES_N; i++)
            if (p.x >= ZONES[i].x && p.x < ZONES[i].x + ZONES[i].w && p.y >= ZONES[i].y && p.y < ZONES[i].y + ZONES[i].h) { link_tap(ZONES[i].name); break; }
        return;
    }
    if (lv_event_get_target(e) == face || lv_event_get_target(e) == av_img) {   // her face: a tap on it is not "away" (that is a picture, and a game's hold);
        lv_point_t p;                              // the centre square of it is the talk tap, nothing drawn and no hole in her
        lv_area_t a;                               // strips: the circle went (Krem, 2026-11-03), people with an avatar know
        lv_indev_get_point(lv_indev_active(), &p);
        lv_obj_get_coords(lv_event_get_target(e), &a);
        int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
        if (p.x >= cx - FACE_TAP / 2 && p.x < cx + FACE_TAP / 2 && p.y >= cy - FACE_TAP / 2 && p.y < cy + FACE_TAP / 2) buttons_tap("middle");
        return;
    }
    if (images[UI_PICTURE].px && !images[UI_PICTURE].filling) {
        if (is_stream(&images[UI_PICTURE])) {
            stream_hidden = true;
            stream_hide_until_us = esp_timer_get_time() + STREAM_HIDE_S * 1000000LL;
        }
        picture_end();
    }
}

// Held on a tapping stream: the way out that needs nothing of Sapphire.
static void on_picture_held(lv_event_t *e)
{
    if (!stream_taps) return;
    stream_left_until_us = esp_timer_get_time() + STREAM_LEFT_S * 1000000LL;
    stream_hidden = true;
    stream_hide_until_us = esp_timer_get_time() + STREAM_HIDE_S * 1000000LL;
    picture_end();
    ESP_LOGI(TAG, "held: the game is left");
}

static void on_place(lv_event_t *e) { buttons_tap((const char *)lv_event_get_user_data(e)); }

int ui_zones(const ui_zone_t **zones) { *zones = ZONES; return ZONES_N; }

// --- the chat picker: the chat's name opens it ------------------------------------------

static void scrim_down(void)
{
    if (picker_scrim) { lv_obj_delete(picker_scrim); picker_scrim = NULL; }
}
static void on_scrim(lv_event_t *e) { lv_dropdown_close(picker); }
static void picker_close(void)
{
    if (picker && lv_dropdown_is_open(picker)) lv_dropdown_close(picker);
    scrim_down();
}
static void on_picker(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) scrim_down();
    else if (code == LV_EVENT_VALUE_CHANGED) {
        char name[64];
        lv_dropdown_get_selected_str(picker, name, sizeof name);
        if (name[0] && strcmp(name, chat_name)) link_pick_chat(name);
    }
}

bool ui_chats_open(const char *joined, const char *current)
{
    if (!ok || !joined || !joined[0] || !take()) return false;
    scrim_down();
    if (ui_app() != APP_VOICE) lv_tileview_set_tile_by_index(tv, APP_VOICE, 0, LV_ANIM_OFF);   // the picker lives on VOICE (it opened on AVATAR, off the glass)
    lv_dropdown_set_options(picker, joined);
    int32_t at = current && current[0] ? lv_dropdown_get_option_index(picker, current) : -1;
    lv_dropdown_set_selected(picker, at >= 0 ? at : 0);
    picker_scrim = plain(lv_layer_top());
    lv_obj_set_size(picker_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(picker_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(picker_scrim, LV_OPA_50, 0);
    lv_obj_set_clickable(picker_scrim, true);
    lv_obj_add_event_cb(picker_scrim, on_scrim, LV_EVENT_CLICKED, NULL);
    lv_dropdown_open(picker);
    lv_obj_t *list = lv_dropdown_get_list(picker);
    lv_obj_update_layout(list);
    lv_obj_align_to(list, picker, LV_ALIGN_OUT_TOP_MID, 0, 0);
    wake_screen();
    lvgl_port_unlock();
    return true;
}

// --- the dial: a corner held opens the slider of what that corner does (volume, the screen's
// brightness, the light's). One card, one slider; a tap beside it closes it. Built when asked
// for and deleted when left, like the Waveshare's.
enum { DIAL_VOLUME, DIAL_SCREEN, DIAL_LIGHT };
static const char *DIAL_NAME[] = {"Volume", "Screen", "Light"};

static void on_dial_close(lv_event_t *e) { lv_obj_delete(dial_card); dial_card = NULL; }

static void on_dial(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (dial_kind == DIAL_VOLUME) audio_set_volume(v);
    else if (dial_kind == DIAL_SCREEN) { settings.brightness = v; board_backlight(v); settings_save_soon(); }
    else { settings.ring = v; settings_save_soon(); if (!light_is_dark()) light_hold(&LIGHT_PLAIN, 3); }
    lv_label_set_text_fmt(dial_lbl, "%s  %d%%", DIAL_NAME[dial_kind], v);
}

static void on_place_held(lv_event_t *e)
{
    if (dial_card) return;
    char job[BUTTON_JOB];
    if (!buttons_does((const char *)lv_event_get_user_data(e), PRESS_SHORT, job, sizeof job)) return;
    int least = 0, now;
    if (!strcmp(job, "mute") || !strcmp(job, "louder") || !strcmp(job, "quieter")) { dial_kind = DIAL_VOLUME; now = audio_volume(); }
    else if (!strcmp(job, "screen")) { dial_kind = DIAL_SCREEN; now = settings.brightness; least = 5; }
    else if (!strcmp(job, "light")) { dial_kind = DIAL_LIGHT; now = settings.ring; least = RING_LEAST; }
    else return;
    dial_card = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(dial_card);
    lv_obj_set_scrollable(dial_card, false);
    lv_obj_set_size(dial_card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(dial_card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dial_card, LV_OPA_80, 0);
    lv_obj_set_clickable(dial_card, true);
    lv_obj_add_event_cb(dial_card, on_dial_close, LV_EVENT_CLICKED, NULL);
    dial_lbl = lv_label_create(dial_card);
    lv_obj_set_style_text_font(dial_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(dial_lbl, C_TEXT, 0);
    lv_label_set_text_fmt(dial_lbl, "%s  %d%%", DIAL_NAME[dial_kind], now);
    lv_obj_align(dial_lbl, LV_ALIGN_CENTER, 0, -48);
    lv_obj_t *sl = lv_slider_create(dial_card);
    lv_obj_set_size(sl, BOARD_W - 80, 24);
    lv_obj_align(sl, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_ext_click_area(sl, 26);
    lv_obj_set_style_bg_color(sl, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, C_ON, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 9, LV_PART_KNOB);
    lv_slider_set_range(sl, least, 100);
    lv_slider_set_value(sl, now, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, on_dial, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *hint = lv_label_create(dial_card);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, C_DIM, 0);
    lv_label_set_text(hint, "tap outside to close");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// --- the settings app: its widgets act at once, on this task (stack in PSRAM: no flash writes) ---

static void on_bright(lv_event_t *e) { settings.brightness = lv_slider_get_value(lv_event_get_target(e)); board_backlight(settings.brightness); settings_save_soon(); }
static void on_vol(lv_event_t *e) { audio_set_volume(lv_slider_get_value(lv_event_get_target(e))); }
#if BOARD_HAS_FILL
static void on_fill(lv_event_t *e) { board_fill_light(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)); }
#endif
static void on_aec_sw(lv_event_t *e) { audio_aec_enable(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)); }
#if BOARD_CAN_SLEEP
static void on_sleep_btn(lv_event_t *e) { power_sleep_soon(); }
#endif
static void on_restart_btn(lv_event_t *e) { power_restart_soon(); }

static void settings_sync(void)
{
    static int last_b = -1, last_v = -1;
    if (settings.brightness != last_b) lv_slider_set_value(bright_sl, last_b = settings.brightness, LV_ANIM_OFF);
    if (audio_volume() != last_v) lv_slider_set_value(vol_sl, last_v = audio_volume(), LV_ANIM_OFF);
    if (fill_sw) lv_obj_set_state(fill_sw, LV_STATE_CHECKED, board_fill_light_on());
    lv_obj_set_state(aec_sw, LV_STATE_CHECKED, audio_aec_enabled());
}

// --- the tick: the halo's color, the state, the clock, the backlight ----------------------

static void draw_toggles(void)
{
    static int was = -1;
    int now = (wake_enabled() ? 1 : 0) | (audio_muted() ? 0 : 2) | (light_is_dark() ? 0 : 4) | (screen_on ? 8 : 0);
    if (was == now) return;
    was = now;
    lv_obj_set_style_bg_color(light_btn, light_is_dark() ? C_BTN_OFF : C_ON, 0);
    lv_obj_set_style_text_color(light_lbl, light_is_dark() ? C_OFF : lv_color_white(), 0);
    lv_obj_set_style_bg_color(screen_btn, screen_on ? C_ON : C_BTN_OFF, 0);
    lv_obj_set_style_text_color(screen_lbl, screen_on ? lv_color_white() : C_OFF, 0);
    lv_obj_set_style_bg_color(wake_btn, wake_enabled() ? C_ON : C_BTN_OFF, 0);
    lv_obj_set_style_text_color(wake_lbl, wake_enabled() ? lv_color_white() : C_OFF, 0);
    lv_obj_set_style_bg_color(mute_btn, audio_muted() ? C_BTN_OFF : C_ON, 0);
    lv_label_set_text(mute_lbl, audio_muted() ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(mute_lbl, audio_muted() ? C_OFF : lv_color_white(), 0);
}

static void draw_clock(void)
{
    if (status_text[0]) { lv_label_set_text(clock_lbl, status_text); return; }
    time_t now = time(NULL);
    if (now < 1600000000) { lv_label_set_text(clock_lbl, "--:--"); return; }
    struct tm t;
    localtime_r(&now, &t);
    char text[8];
    strftime(text, sizeof text, "%H:%M", &t);
    lv_label_set_text(clock_lbl, text);
}

static void page_fill(int which);

static void on_tick(lv_timer_t *t)
{
    beat_us = esp_timer_get_time();
    if (stream_hidden && esp_timer_get_time() > stream_hide_until_us && !server_talking() && !avatar_voice() && !light_turn_on())
        stream_hidden = false;
    static uint32_t was = 0xffffffff;
    uint8_t r, g, b;
    light_current(&r, &g, &b);
    const char *cue = light_cue_now();
    bool busy = strcmp(cue, "idle") && strcmp(cue, "shown") && strcmp(cue, "starting") && strcmp(cue, "off");
    if (!busy && trim >= 0) { r = trim >> 16; g = trim >> 8 & 255; b = trim & 255; }
    uint32_t now_rgb = (r >> 3) << 16 | (g >> 3) << 8 | (b >> 3);
    if (now_rgb != was) {
        was = now_rgb;
        lv_obj_set_style_bg_color(halo, lv_color_make(r, g, b), 0);
        lv_obj_set_style_bg_color(glow, lv_color_make(r, g, b), 0);
        lv_obj_set_style_border_color(face, lv_color_make(r, g, b), 0);
    }
    if (strcmp(cue, last_cue)) {
        strlcpy(last_cue, cue, sizeof last_cue);
        lv_label_set_text(state_lbl, busy ? cue : "");
        if (busy && screen_on) wake_screen();
        ear_set(!strcmp(cue, "listening"));            // the bar painted into her face while she listens
    }
    draw_toggles();
    static int n;
    if (++n % 10 == 0) {
        draw_clock();
        lv_obj_t *active = lv_tileview_get_tile_active(tv);
        for (int i = 0; i < APPS; i++) if (active == page[i]) page_fill(i);
        if (active == app[APP_SETUP]) settings_sync();
        if (active == app[APP_BACKUP]) backups_fill();
    }
    uint32_t alone = lv_display_get_inactive_time(disp);
    // Her tile up or not, told to Sapphire once it holds 300 ms (a slide passes through it): her idle crosses
    // the air only while she is shown, and Sapphire hears the change at once (not by a frame every 2 s)
    static int told = -1, seen = -1;
    static int64_t seen_at, told_at;
    int now_shown = av_shown = av_shown_now();
    int64_t t_us = esp_timer_get_time();
    if (now_shown != seen) { seen = now_shown; seen_at = t_us; }
    else if (t_us - seen_at > 300000 && (seen != told || t_us - told_at > 60000000) && link_shown(seen)) { told = seen; told_at = t_us; }   // and again each minute: a lost word never strands her
    // the tile it was left on, kept once it has stayed 30 s (a few NVS writes a day, never one a swipe)
    static int last_app = -1;
    static int64_t app_since;
    int app_now = ui_app();
    if (app_now != last_app) { last_app = app_now; app_since = esp_timer_get_time(); }
    else if (app_now != settings.home_app && esp_timer_get_time() - app_since > 30000000) { settings.home_app = app_now; settings_save_soon(); }
    // Outside awake hours an idle screen goes dark, not dim (its day: the Devices page's Power tab)
    bool night = !light_in_hours();
    if (screen_on && !busy && !asleep && night && dimmed) {
        asleep = true;
        board_backlight(0);
        picker_close();
    } else if (screen_on && !busy && !asleep && settings.off_after_min && alone > (uint32_t)settings.off_after_min * 60000) {
        asleep = true;
        board_backlight(0);
        picker_close();
    } else if (screen_on && !busy && !asleep && !dimmed && settings.dim_after_s && alone > (uint32_t)settings.dim_after_s * 1000) {
        dimmed = true;
        board_backlight(settings.dim_percent);
        picker_close();
    }
}

static void on_heard_end(lv_timer_t *t) { lv_obj_set_hidden(heard, true); lv_timer_delete(heard_timer); heard_timer = NULL; }
static void on_toast_end(lv_timer_t *t) { lv_obj_set_hidden(toast, true); lv_timer_delete(toast_timer); toast_timer = NULL; }

// --- the backups app: her backups on the card, on his neck -------------------------------------
// A row per backup (the newest six): when it was made, how big, and whether it read back whole.
// Back up now asks Sapphire for one (link_ask: she makes it, seals it, sends it; her answer
// shows as a toast); a landing one shows as a bar. Check reads every backup back against its hash.

// "sapphire_2026-11-02_163533_manual.sapphirebak" -> "Nov 02 16:35 manual"
static void backup_words(const char *name, char *out, size_t size)
{
    static const char *MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y, mo, d, hh, mm; char kind[16] = "";
    if (sscanf(name, "sapphire_%d-%d-%d_%2d%2d%*2d_%15[a-z]", &y, &mo, &d, &hh, &mm, kind) >= 5 && mo >= 1 && mo <= 12)
        snprintf(out, size, "%s %02d %02d:%02d %s", MON[mo - 1], d, hh, mm, kind);
    else strlcpy(out, name, size);
}

static void backups_list(void)
{
    struct { char name[STORAGE_NAME_MAX + 1]; long mtime; uint64_t size; } rows[6];
    int n = 0;
    DIR *d = storage_ok() ? opendir(storage_dir()) : NULL;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t len = strlen(e->d_name);
            if (len < 13 || strcmp(e->d_name + len - 12, ".sapphirebak")) continue;
            char path[STORAGE_NAME_MAX + 64];
            struct stat st;
            if (!storage_path(e->d_name, path, sizeof path) || stat(path, &st)) continue;
            int at = n < 6 ? n : 5;
            if (n == 6 && (long)st.st_mtime <= rows[5].mtime) continue;
            while (at > 0 && rows[at - 1].mtime < (long)st.st_mtime) { if (at < 6) rows[at] = rows[at - 1]; at--; }
            strlcpy(rows[at].name, e->d_name, sizeof rows[at].name);
            rows[at].mtime = st.st_mtime;
            rows[at].size = st.st_size;
            if (n < 6) n++;
        }
        closedir(d);
    }
    lv_obj_clean(bk_rows);
    for (int i = 0; i < n && i < bk_shown; i++) {
        lv_obj_t *l = small_label(bk_rows, &lv_font_montserrat_14, C_TEXT);
        lv_obj_set_width(l, BOARD_W - 28);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(l, 0, i * 22);
        char when[40], one[96];
        backup_words(rows[i].name, when, sizeof when);
        int ok = storage_checked(rows[i].name);
        snprintf(one, sizeof one, "%s  %llu MB  %s", when, (unsigned long long)(rows[i].size >> 20), ok == 1 ? LV_SYMBOL_OK : ok == 0 ? "DAMAGED" : "?");
        lv_label_set_text(l, one);
        if (ok == 0) lv_obj_set_style_text_color(l, lv_color_hex(0xff5050), 0);
    }
    if (!n) {
        lv_obj_t *l = small_label(bk_rows, &lv_font_montserrat_14, C_DIM);
        lv_label_set_text(l, storage_ok() ? "No backups on the card yet." : "No card.");
    }
}

static void backups_fill(void)
{
    storage_state_t st;
    storage_state(&st);
    uint64_t free_b = 0, total = 0;
    char head[96];
    if (!st.mounted) strlcpy(head, "No card in the slot.", sizeof head);
    else {
        storage_room(&free_b, &total);
        snprintf(head, sizeof head, "%d on the card, %.1f of %.1f GB free", st.count, free_b / 1073741824.0, total / 1073741824.0);
    }
    lv_label_set_text(bk_head, head);
    if (st.writing) {
        int pct = st.expect ? (int)(st.got * 100 / st.expect) : 0;
        lv_bar_set_value(bk_bar, pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(bk_prog, "one arriving: %d%%, %llu of %llu MB", pct, (unsigned long long)(st.got >> 20), (unsigned long long)(st.expect >> 20));
        lv_obj_set_hidden(bk_bar, false);
    } else {
        lv_obj_set_hidden(bk_bar, true);
        if (st.failed) lv_label_set_text(bk_prog, "the last one did NOT land");
        else if (st.checking) lv_label_set_text_fmt(bk_prog, "checking %d of %d", st.checked, st.to_check);
        else if (st.damaged) lv_label_set_text_fmt(bk_prog, "%d DAMAGED", st.damaged);
        else if (st.count) lv_label_set_text(bk_prog, st.unchecked ? "ok" : "all read back whole");
        else lv_label_set_text(bk_prog, "");
    }
    if (st.mounted && link_connected() && !st.writing) lv_obj_remove_state(bk_btn, LV_STATE_DISABLED); else lv_obj_add_state(bk_btn, LV_STATE_DISABLED);
    if (st.mounted && st.count && !st.writing) lv_obj_remove_state(bk_check, LV_STATE_DISABLED); else lv_obj_add_state(bk_check, LV_STATE_DISABLED);
    if (st.checking != bk_checking) lv_label_set_text(lv_obj_get_child(bk_check, 0), st.checking ? LV_SYMBOL_STOP " Stop" : LV_SYMBOL_OK " Check");   // the same button stops a check
    if (st.count != bk_count || (bk_writing && !st.writing) || (bk_checking && !st.checking)) backups_list();
    bk_count = st.count;
    bk_writing = st.writing;
    bk_checking = st.checking;
}

static void on_backup_now(lv_event_t *e) { link_ask("backup"); lv_obj_add_state(bk_btn, LV_STATE_DISABLED); ui_toast("Asking Sapphire for a backup...", 4); }
static void on_check_card(lv_event_t *e)
{
    if (storage_check_cancel()) { ui_toast("Stopping the check", 2); return; }
    if (!storage_check_now(STORAGE_CHECK_ALL, NULL)) ui_toast("No card, or nothing to check", 3);
}

// --- the status pages ---------------------------------------------------------------------

// The board's power, a line: the pendant's rail, fill light and sleeps; a board with none of them says so
static const char *power_line(void)
{
    static char line[96];
#if BOARD_CAN_SLEEP
    snprintf(line, sizeof line, "rail  %s%s%s\nsleep after  %d min   slept %d times", board_rail_on() ? "on" : "off",
             BOARD_HAS_FILL ? "   fill light  " : "", BOARD_HAS_FILL ? (board_fill_light_on() ? "on" : "off") : "",
             settings.sleep_after_min, power_sleeps());
#else
    board_battery_t b;
    if (board_battery_read(&b) && b.battery) snprintf(line, sizeof line, "battery  %d%%  %d mV", b.percent, b.mv);
    else snprintf(line, sizeof line, "power  always on (no rail, no sleep)");
#endif
    return line;
}

static void page_fill(int which)
{
    char text[700], up[24];
    uint32_t s = esp_timer_get_time() / 1000000;
    if (s >= 3600) snprintf(up, sizeof up, "%uh %um", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
    else snprintf(up, sizeof up, "%um %us", (unsigned)(s / 60), (unsigned)(s % 60));
    if (which == APP_BACKUP) {
        storage_state_t st;
        storage_state(&st);
        uint64_t free_b = 0, total = 0;
        if (st.mounted) storage_room(&free_b, &total);
        snprintf(text, sizeof text,
                 "card  %s\nfree  %.2f GB of %.2f\nbackups  %d\ndamaged  %d   unchecked  %d\nnewest  %s\narriving  %s\nchecks  %s %d of %d\nlast error  %s%s",
                 st.mounted ? "mounted" : "none", free_b / 1073741824.0, total / 1073741824.0, st.count, st.damaged, st.unchecked,
                 st.newest[0] ? st.newest : "-", st.writing ? "yes" : "no", st.checking ? "running" : "done", st.checked, st.to_check,
                 storage_last_error()[0] ? storage_last_error() : "none",
                 SHORT_GLASS ? "" : "\n\nSapphire sends one every night and keeps\nas many here as the Devices page says.");
    } else if (which == APP_VOICE) {
        snprintf(text, sizeof text,
                 "%s\n\nchat  %s\nbrain  %s\n\n%s   %d dBm\nSapphire  %s\n\nwake word  %s%s\nmic gain  %.0f dB\necho  %s, ref %d ms\nvolume  %d%%",
                 settings.name, chat_name[0] ? chat_name : "-", brain_name[0] ? brain_name : "-",
                 net_ip()[0] ? net_ip() : "no WiFi", net_rssi(), link_connected() ? "linked" : "NOT linked",
                 wake_model_name(), wake_enabled() ? "" : " (off)", audio_mic_gain(),
                 audio_aec_enabled() ? "on" : "off", audio_aec_delay_ms(), audio_volume());
        int painted, replaced, gm, gx, gl;
        ui_stream_counts(&painted, &replaced);
        ui_stream_gaps(&gm, &gx, &gl);
        size_t at = strlen(text);
        snprintf(text + at, sizeof text - at, "\nface  %d frames, beat %d/s, gap %d ms\ncamera  %s, %d ms",
                 painted, UI_STREAM_FPS, gm, camera_present() ? camera_sensor_name() : "none", camera_last_encode_ms());
    } else if (which == APP_AVATAR) {
        avatar_stats_t a;
        avatar_stats(&a);
        int painted, replaced;
        ui_stream_counts(&painted, &replaced);
        snprintf(text, sizeof text,
                 "her stream\n\npackets  %d   holes  %d\nframes  %d   late  %d\nqueued  %d   buffer  %d\ngap  %d ms   decode  %d ms\npaint  %d ms   %s\n\npictures  %d frames%s",
                 a.packets, a.underruns, a.frames, a.late, a.queued, settings.av_buffer, a.gap_ms, a.decode_ms, a.paint_ms, a.playing ? "playing" : "quiet", painted,
                 SHORT_GLASS ? "" : "\n\nHer voice and her face come\nhere as one stream, on one\nclock. A tap on her is the talk tap.");
    } else if (which == APP_ACTIONS) {
        size_t at = 0;
        text[0] = 0;
        for (int i = 0; i < VBUTTONS && at < sizeof text; i++) {
            char job[BUTTON_JOB];
            bool set = buttons_does(VNAMES[i], PRESS_SHORT, job, sizeof job);
            at += snprintf(text + at, sizeof text - at, "%c  %s%s%s%s\n", 'A' + i, !set ? "not set" : !strcmp(job, "tell") ? "message" : job,
                           vlabels[i][0] ? "  \"" : "", vlabels[i], vlabels[i][0] ? "\"" : "");
        }
        if (at < sizeof text) snprintf(text + at, sizeof text - at, "Sapphire %s - set in Settings > Devices > Buttons", link_connected() ? "linked" : "NOT linked");
    } else if (which == APP_SETUP) {
        storage_state_t st;
        storage_state(&st);
        cJSON *words = lastwords_json(false);
        const cJSON *why = words ? cJSON_GetObjectItem(words, "reason") : NULL;
        snprintf(text, sizeof text,
                 "program  %s %s   up %s\nRAM  %u KB free (low %u), %u MB spare\ncard  %s%s\n%s\nstarted by  %s\n%s%s",
                 FIRMWARE_MODEL, PROJECT_VER, up,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024)),
                 st.mounted ? "in" : "none", st.mounted && st.count ? ", backups kept" : "",
                 power_line(),
                 lastwords_reset(),
                 cJSON_IsString(why) ? "last down  " : "", cJSON_IsString(why) ? why->valuestring : "");
        cJSON_Delete(words);
    }
    if (SHORT_GLASS) for (char *p; (p = strstr(text, "\n\n")); ) memmove(p, p + 1, strlen(p));   // no blank lines where the glass is 240 tall
    lv_label_set_text(page_lbl[which], text);
}

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

static void pill(lv_obj_t *label)
{
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_50, 0);
    lv_obj_set_style_radius(label, 8, 0);
    lv_obj_set_style_pad_hor(label, 8, 0);
    lv_obj_set_style_pad_ver(label, 2, 0);
}

static lv_obj_t *round_button(lv_obj_t *parent, lv_align_t where, int dx, int dy, const char *place, lv_obj_t **label)
{
    lv_obj_t *b = plain(parent);
    lv_obj_set_size(b, BUTTON, BUTTON);
    lv_obj_align(b, where, dx, dy);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(b, C_BTN_OFF, 0);
    lv_obj_set_style_border_color(b, C_LINE, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_clickable(b, true);
    lv_obj_set_ext_click_area(b, SHORT_GLASS ? 22 : 10);   // the speaker's old corners were 72 px places: a fingertip lands
    lv_obj_add_event_cb(b, on_place, LV_EVENT_SHORT_CLICKED, (void *)place);
    lv_obj_add_event_cb(b, on_place_held, LV_EVENT_LONG_PRESSED, (void *)place);
    *label = small_label(b, &lv_font_montserrat_20, C_OFF);
    lv_obj_center(*label);
    lv_obj_set_clickable(*label, false);
    return b;
}

static lv_obj_t *tile(int col, int row, lv_dir_t dir)
{
    lv_obj_t *t = lv_tileview_add_tile(tv, col, row, dir);
    lv_obj_set_style_bg_color(t, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_set_scrollable(t, false);
    return t;
}

static lv_obj_t *status_page(int which, const char *title)
{
    lv_obj_t *p = tile(which, 1, LV_DIR_TOP);
    lv_obj_t *t = small_label(p, &lv_font_montserrat_16, C_DIM);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 10, 8);
    page_lbl[which] = small_label(p, SHORT_GLASS ? &lv_font_montserrat_12 : &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_width(page_lbl[which], BOARD_W - 20);
    lv_label_set_long_mode(page_lbl[which], LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(page_lbl[which], SHORT_GLASS ? 2 : 3, 0);
    lv_obj_align(page_lbl[which], LV_ALIGN_TOP_LEFT, 10, SHORT_GLASS ? 28 : 32);
    if (!SHORT_GLASS) {                                // a 240-tall glass needs the line for her words: the gesture is the same on every page
        lv_obj_t *hint = small_label(p, &lv_font_montserrat_12, C_DIM);
        lv_label_set_text(hint, "swipe down to go back");
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    }
    return p;
}

static lv_obj_t *row_label(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *l = small_label(parent, &lv_font_montserrat_16, C_TEXT);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 14, y);
    return l;
}

static lv_obj_t *slider(lv_obj_t *parent, int y, int least, int now, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_size(s, BOARD_W - 44, 18);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, 22, y);
    lv_obj_set_ext_click_area(s, 16);
    lv_obj_set_style_bg_color(s, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, C_ON, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 8, LV_PART_KNOB);
    lv_slider_set_range(s, least, 100);
    lv_slider_set_value(s, now, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
}

static lv_obj_t *toggle(lv_obj_t *parent, const char *text, int y, bool on, lv_event_cb_t cb)
{
    row_label(parent, text, y + 6);
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 66, 32);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -14, y);
    lv_obj_set_style_bg_color(sw, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, C_ON, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_state(sw, LV_STATE_CHECKED, on);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_align_t where, int dx, int dy, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 104, 42);
    lv_obj_align(b, where, dx, dy);
    lv_obj_set_style_bg_color(b, C_LINE, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static int64_t tile_changed_us;        // a tile just settled: a finger landing now is the slide's, not a press (Actions)

static void on_tile_changed(lv_event_t *e)
{
    tile_changed_us = esp_timer_get_time();
    if (lv_tileview_get_tile_active(tv) == app[APP_SETUP]) settings_sync();
    lv_display_trigger_activity(disp);
}

// --- ACTIONS: nine buttons of your own (the CYD's Actions page, 1.0.0) -------------------------
// Each is a..i in buttons.c, a tap each; Settings > Devices > Buttons sets what it does. A `tell`
// carries its message's first words to the glass (the events stream's "labels"); a board job
// carries its name; nothing set reads "not set", dim, and a tap says where to set it.

static bool vbutton_words(int i, char *out, size_t size)
{
    char job[BUTTON_JOB];
    if (vlabels[i][0]) { snprintf(out, size, "%s", vlabels[i]); return true; }
    if (buttons_does(VNAMES[i], PRESS_SHORT, job, sizeof job)) { snprintf(out, size, "%c\n%s", 'A' + i, !strcmp(job, "tell") ? "message" : job); return true; }
    snprintf(out, size, "%c\nnot set", 'A' + i);
    return false;
}

static void vbutton_draw(int i)
{
    if (!vbtn[i]) return;
    char words[VLABEL_MAX + 24];
    bool set = vbutton_words(i, words, sizeof words);
    lv_obj_t *l = lv_obj_get_child(vbtn[i], 0);
    lv_label_set_text(l, words);
    lv_obj_set_style_text_color(l, set ? C_TEXT : C_DIM, 0);
}

static void on_vbutton(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (esp_timer_get_time() - tile_changed_us < 300000 || lv_obj_is_scrolling(tv)) return;   // the tail of a slide (Krem hit one sliding in, 2026-11-12)
    char job[BUTTON_JOB];
    if (!buttons_does(VNAMES[i], PRESS_SHORT, job, sizeof job)) { ui_toast("Settings > Devices > Buttons sets what this one does", 5); return; }
    buttons_tap(VNAMES[i]);                // buttons.c runs what the press is set to, on its next poll
}

// A 3x3 that fills the tile under a small title: 70x78 on the pendant, 97x64 on the speaker
static void actions_build(lv_obj_t *t)
{
    lv_obj_t *title = small_label(t, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(title, "Actions");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);
    int bw = (BOARD_W - 16 - 12) / 3, bh = (BOARD_H - 24 - 8 - 16) / 3;
    for (int i = 0; i < VBUTTONS; i++) {
        lv_obj_t *b = lv_button_create(t);
        lv_obj_set_size(b, bw, bh);
        lv_obj_set_pos(b, 8 + i % 3 * (bw + 6), 24 + i / 3 * (bh + 8));
        lv_obj_set_style_bg_color(b, C_BTN_OFF, 0);
        lv_obj_set_style_border_color(b, C_LINE, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, on_vbutton, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);   // a hold does nothing: a swipe that starts on one is a swipe
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, bw - 8);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        vbtn[i] = b;
        vbutton_draw(i);
    }
}

void ui_button_labels(const cJSON *labels)
{
    if (!ok) return;
    bool locked = take();
    for (int i = 0; i < VBUTTONS; i++) {
        const cJSON *w = cJSON_IsObject(labels) ? cJSON_GetObjectItem(labels, VNAMES[i]) : NULL;
        snprintf(vlabels[i], sizeof vlabels[i], "%s", cJSON_IsString(w) ? w->valuestring : "");
        if (locked) vbutton_draw(i);       // without a label: the job's name, or "not set" (what bound says now)
    }
    if (locked) lvgl_port_unlock();
    else labels_due = true;                // the glass was busy: on_tick draws them (they were dropped till the next event, scout)
}

static void build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollable(scr, false);

    tv = lv_tileview_create(scr);
    lv_obj_set_size(tv, BOARD_W, BOARD_H);
    lv_obj_set_style_bg_color(tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(tv, on_tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // --- VOICE ---
    app[APP_VOICE] = tile(APP_VOICE, 0, LV_DIR_HOR | LV_DIR_BOTTOM);
    lv_obj_t *v = app[APP_VOICE];
    scene = lv_image_create(v);
    lv_obj_set_hidden(scene, true);
    clock_lbl = small_label(v, SHORT_GLASS ? &lv_font_montserrat_28 : &lv_font_montserrat_36, C_TEXT);
    pill(clock_lbl);
    lv_obj_align(clock_lbl, LV_ALIGN_TOP_MID, 0, SHORT_GLASS ? 6 : 8);
    lv_obj_set_clickable(clock_lbl, true);
    lv_obj_add_event_cb(clock_lbl, on_place, LV_EVENT_SHORT_CLICKED, (void *)"top");
    state_lbl = small_label(v, &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(state_lbl, LV_ALIGN_TOP_MID, 0, SHORT_GLASS ? 44 : 58);
    glow = plain(v);
    lv_obj_set_size(glow, SHORT_GLASS ? 76 : 110, SHORT_GLASS ? 76 : 110);   // the speaker's proven sizes on a 240-tall glass
    lv_obj_center(glow);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_30, 0);
    lv_obj_set_clickable(glow, false);
    halo = plain(v);
    lv_obj_set_size(halo, SHORT_GLASS ? 44 : 64, SHORT_GLASS ? 44 : 64);
    lv_obj_center(halo);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_COVER, 0);
    lv_obj_set_clickable(halo, true);
    lv_obj_set_ext_click_area(halo, 30);
    lv_obj_add_event_cb(halo, on_place, LV_EVENT_SHORT_CLICKED, (void *)"middle");
    heard = small_label(v, &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_width(heard, SHORT_GLASS ? BOARD_W - 2 * BUTTON - 24 : BOARD_W - 24);   // between the corner buttons where the glass is short
    lv_label_set_long_mode(heard, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(heard, LV_TEXT_ALIGN_CENTER, 0);
    pill(heard);
    lv_obj_align(heard, LV_ALIGN_BOTTOM_MID, 0, SHORT_GLASS ? -40 : -66);
    lv_obj_set_hidden(heard, true);
    chat_lbl = small_label(v, &lv_font_montserrat_16, C_TEXT);
    pill(chat_lbl);
    lv_obj_set_style_max_width(chat_lbl, BOARD_W - 2 * BUTTON - 24, 0);
    lv_label_set_long_mode(chat_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(chat_lbl, LV_ALIGN_BOTTOM_MID, 0, SHORT_GLASS ? -8 : -14);
    lv_obj_set_clickable(chat_lbl, true);
    lv_obj_set_ext_click_area(chat_lbl, 12);
    lv_obj_add_event_cb(chat_lbl, on_place, LV_EVENT_SHORT_CLICKED, (void *)"bottom");
    lv_obj_set_hidden(chat_lbl, true);
    light_btn = round_button(v, LV_ALIGN_TOP_LEFT, 8, 8, "top-left", &light_lbl);
    lv_label_set_text(light_lbl, LV_SYMBOL_TINT);
    screen_btn = round_button(v, LV_ALIGN_TOP_RIGHT, -8, 8, "top-right", &screen_lbl);
    lv_label_set_text(screen_lbl, LV_SYMBOL_POWER);
    wake_btn = round_button(v, LV_ALIGN_BOTTOM_LEFT, 8, -8, "bottom-left", &wake_lbl);
    lv_label_set_text(wake_lbl, LV_SYMBOL_BELL);
    mute_btn = round_button(v, LV_ALIGN_BOTTOM_RIGHT, -8, -8, "bottom-right", &mute_lbl);
    lv_label_set_text(mute_lbl, LV_SYMBOL_VOLUME_MAX);
    picker = lv_dropdown_create(v);
    lv_obj_remove_style_all(picker);
    lv_obj_set_size(picker, BOARD_W - 20, 28);
    lv_obj_align(picker, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_clickable(picker, false);
    lv_dropdown_set_dir(picker, LV_DIR_TOP);
    lv_dropdown_set_symbol(picker, NULL);
    lv_dropdown_set_text(picker, "");
    lv_dropdown_set_options(picker, "");
    lv_obj_add_event_cb(picker, on_picker, LV_EVENT_ALL, NULL);
    lv_obj_t *list = lv_dropdown_get_list(picker);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(list, C_TEXT, 0);
    lv_obj_set_style_text_line_space(list, 24, 0);
    lv_obj_set_style_bg_color(list, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_90, 0);
    lv_obj_set_style_border_color(list, C_LINE, 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_radius(list, 8, 0);
    lv_obj_set_style_pad_ver(list, 6, 0);
    lv_obj_set_style_pad_hor(list, 10, 0);
    lv_obj_set_style_max_height(list, 4 * 46 + 12, 0);
    lv_obj_set_style_max_width(list, BOARD_W - 16, 0);
    lv_obj_set_style_bg_color(list, C_ON, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);


    // --- SETTINGS ---
    app[APP_SETUP] = tile(APP_SETUP, 0, LV_DIR_HOR | LV_DIR_BOTTOM);
    lv_obj_t *s = app[APP_SETUP];
#if BOARD_CAN_SLEEP
    button(s, LV_SYMBOL_POWER " Sleep", LV_ALIGN_TOP_LEFT, 12, SHORT_GLASS ? 8 : 12, on_sleep_btn);
#endif
    button(s, LV_SYMBOL_REFRESH " Restart", LV_ALIGN_TOP_RIGHT, -12, SHORT_GLASS ? 8 : 12, on_restart_btn);
    int y = SHORT_GLASS ? 58 : 66, gap = SHORT_GLASS ? 54 : 58;   // label, its slider 26 below; the next row a gap on
    row_label(s, "Brightness", y);
    bright_sl = slider(s, y + 26, 5, settings.brightness, on_bright);
    y += gap;
    row_label(s, "Volume", y);
    vol_sl = slider(s, y + 26, 0, settings.volume, on_vol);
    y += gap + (SHORT_GLASS ? 0 : 6);
#if BOARD_HAS_FILL
    fill_sw = toggle(s, "Fill light", y, board_fill_light_on(), on_fill);
    y += 46;
#endif
    aec_sw = toggle(s, "Echo cancel", y, audio_aec_enabled(), on_aec_sw);

    // --- BACKUPS ---
    app[APP_BACKUP] = tile(APP_BACKUP, 0, LV_DIR_RIGHT | LV_DIR_BOTTOM);
    lv_obj_t *b = app[APP_BACKUP];
    lv_obj_t *bt = small_label(b, &lv_font_montserrat_16, C_DIM);
    lv_label_set_text(bt, "Backups");
    lv_obj_align(bt, LV_ALIGN_TOP_LEFT, 14, 10);
    bk_head = small_label(b, &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_width(bk_head, BOARD_W - 28);
    lv_label_set_long_mode(bk_head, LV_LABEL_LONG_WRAP);
    lv_obj_align(bk_head, LV_ALIGN_TOP_LEFT, 14, 36);
    // from the bottom up: the buttons, the progress line, the bar, then as many rows as fit (the pendant 6, the speaker 4)
    int btn_top = BOARD_H - 52, prog_y = btn_top - 20, bar_y = prog_y - 12;
    bk_shown = (bar_y - 68) / 22;
    if (bk_shown > BK_ROWS_MAX) bk_shown = BK_ROWS_MAX;
    bk_rows = plain(b);
    lv_obj_set_size(bk_rows, BOARD_W - 28, bk_shown * 22);
    lv_obj_set_pos(bk_rows, 14, 64);
    bk_bar = lv_bar_create(b);
    lv_obj_set_size(bk_bar, BOARD_W - 28, 10);
    lv_obj_set_pos(bk_bar, 14, bar_y);
    lv_bar_set_range(bk_bar, 0, 100);
    lv_obj_set_style_bg_color(bk_bar, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bk_bar, C_ON, LV_PART_INDICATOR);
    lv_obj_set_hidden(bk_bar, true);
    bk_prog = small_label(b, &lv_font_montserrat_14, C_DIM);
    lv_obj_set_width(bk_prog, BOARD_W - 28);
    lv_label_set_long_mode(bk_prog, LV_LABEL_LONG_DOT);
    lv_obj_align(bk_prog, LV_ALIGN_TOP_LEFT, 14, prog_y);
    bk_btn = button(b, LV_SYMBOL_SD_CARD " Back up", LV_ALIGN_BOTTOM_LEFT, 12, -10, on_backup_now);
    bk_check = button(b, LV_SYMBOL_OK " Check", LV_ALIGN_BOTTOM_RIGHT, -12, -10, on_check_card);
    backups_fill();

    // --- the status pages under them ---
    // --- AVATAR: hers alone (0.2.0) ---
    app[APP_AVATAR] = tile(APP_AVATAR, 0, LV_DIR_HOR | LV_DIR_BOTTOM);
    lv_obj_t *av = app[APP_AVATAR];
    av_px = heap_caps_calloc((size_t)BOARD_AV_W * BOARD_AV_H, 2, MALLOC_CAP_SPIRAM);
    av_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    av_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    av_dsc.header.w = BOARD_AV_W;
    av_dsc.header.h = BOARD_AV_H;
    av_dsc.header.stride = BOARD_AV_W * 2;
    av_dsc.data_size = (uint32_t)BOARD_AV_W * BOARD_AV_H * 2;
    av_dsc.data = av_px;
    av_img = lv_image_create(av);
    lv_image_set_src(av_img, &av_dsc);
    lv_obj_set_size(av_img, BOARD_AV_W, BOARD_AV_H);
    lv_obj_center(av_img);
    lv_obj_set_clickable(av_img, true);
    lv_obj_add_event_cb(av_img, on_picture_tap, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_set_hidden(av_img, true);
    // (the listen bar under her face went in 1.0.0: her box is the whole glass now, so a bar would be a hole in
    // every blit. The light says listening: the ring on the speaker, the status LED on the pendant)
    face = lv_image_create(av);                    // the picture door's stream (a game's table), the same place
    lv_obj_set_size(face, FACE, FACE);
    lv_obj_center(face);
    lv_obj_set_clickable(face, true);
    lv_obj_add_event_cb(face, on_picture_tap, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(face, on_picture_held, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_set_hidden(face, true);

    actions_build(tile(APP_ACTIONS, 0, LV_DIR_LEFT | LV_DIR_BOTTOM));

    page[APP_BACKUP] = status_page(APP_BACKUP, "the card");
    page[APP_VOICE] = status_page(APP_VOICE, "this board");
    page[APP_SETUP] = status_page(APP_SETUP, "the board");
    page[APP_AVATAR] = status_page(APP_AVATAR, "her stream");
    page[APP_ACTIONS] = status_page(APP_ACTIONS, "its buttons");
    lv_tileview_set_tile_by_index(tv, settings.home_app >= 0 && settings.home_app < APPS ? settings.home_app : APP_VOICE, 0, LV_ANIM_OFF);   // the tile it was left on

    // --- over everything: her still picture on its shade, her line across the top ---
    shade = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(shade);
    lv_obj_set_size(shade, BOARD_W, BOARD_H);
    lv_obj_set_style_bg_opa(shade, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(shade, lv_color_black(), 0);
    lv_obj_set_hidden(shade, true);
    lv_obj_set_clickable(shade, true);
    lv_obj_add_event_cb(shade, on_picture_tap, LV_EVENT_SHORT_CLICKED, NULL);
    picture = lv_image_create(lv_layer_top());
    lv_obj_set_hidden(picture, true);
    lv_obj_set_clickable(picture, true);
    lv_obj_add_event_cb(picture, on_picture_tap, LV_EVENT_SHORT_CLICKED, NULL);
    toast = lv_label_create(lv_layer_top());
    lv_obj_set_width(toast, LV_PCT(100));
    lv_label_set_long_mode(toast, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(toast, &lv_font_montserrat_16, 0);
    lv_obj_set_style_bg_color(toast, lv_palette_darken(LV_PALETTE_BLUE, 3), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(toast, 8, 0);
    lv_obj_set_style_text_color(toast, C_TEXT, 0);
    lv_obj_set_style_text_align(toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_hidden(toast, true);

    draw_toggles();
    draw_clock();
    tick = lv_timer_create(on_tick, 100, NULL);
}

static int64_t echo_until_us;
static bool echo_was;
void ui_touch_echo(int seconds) { echo_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000; }

#define TOUCH_LOG 12
static struct { int x, y; int64_t at; } touch_log[TOUCH_LOG];
static int touch_log_at;

cJSON *ui_touch_log(void)
{
    cJSON *a = cJSON_CreateArray();
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < TOUCH_LOG; i++) {
        int k = (touch_log_at - 1 - i + TOUCH_LOG * 2) % TOUCH_LOG;
        if (!touch_log[k].at) break;
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "x", touch_log[k].x);
        cJSON_AddNumberToObject(t, "y", touch_log[k].y);
        cJSON_AddNumberToObject(t, "ms_ago", (double)((now - touch_log[k].at) / 1000));
        cJSON_AddItemToArray(a, t);
    }
    return a;
}

// Bench (1.0.0): every touch sample while a finger is down, and the release, for the swipe that went the
// wrong way 1 time in 4 (Krem, 2026-11-12): /health screen.touch_trace [[x, y, ms since the last], ...]
#define TRACE_N 96
static struct { int16_t x, y; uint16_t ms; } trace[TRACE_N];
static int trace_at;
static int64_t trace_last_us;

static void trace_add(int x, int y)
{
    int64_t now = esp_timer_get_time();
    int ms = trace_last_us ? (int)((now - trace_last_us) / 1000) : 0;
    trace_last_us = now;
    trace[trace_at % TRACE_N].x = x; trace[trace_at % TRACE_N].y = y; trace[trace_at % TRACE_N].ms = ms > 65535 ? 65535 : ms;
    trace_at++;
}

cJSON *ui_touch_trace(void)
{
    cJSON *a = cJSON_CreateArray();
    int n = trace_at < TRACE_N ? trace_at : TRACE_N;
    int at = trace_at;                     // once: the touch task writes on
    n = at < TRACE_N ? at : TRACE_N;
    for (int i = at - n; i < at; i++) {
        cJSON *p = cJSON_CreateArray();
        cJSON_AddItemToArray(p, cJSON_CreateNumber(trace[i % TRACE_N].x));
        cJSON_AddItemToArray(p, cJSON_CreateNumber(trace[i % TRACE_N].y));
        cJSON_AddItemToArray(p, cJSON_CreateNumber(trace[i % TRACE_N].ms));
        cJSON_AddItemToArray(a, p);
    }
    return a;
}

// --- the touch, sampled on its own task (1.0.0) ---------------------------------------------------
// The chip was read by LVGL's task, which draws: during a tile slide a frame takes ~50 ms, so touch reads
// fell from every 12 ms to every 40-120 ms, LVGL judged a flick from two or three samples, and a little
// wobble at lift-off threw 1 slide in 4-10 the WRONG way (Krem, 2026-11-12, touch_trace). Now a task of its
// own reads it every 10 ms into a ring; LVGL drains the ring (continue_reading), so it sees every sample at
// its true spacing; and at the lift, trailing samples that turn back against the last ~100 ms are dropped.
#define TOUCH_RING 48
typedef struct { int16_t x, y; bool down; } touch_pt_t;
static touch_pt_t touch_ring[TOUCH_RING];
static int touch_head, touch_tail;      // head: next to write; tail: next for LVGL
static portMUX_TYPE touch_mux = portMUX_INITIALIZER_UNLOCKED;
static touch_pt_t touch_last = {0, 0, false};

static void touch_push(touch_pt_t p)
{
    portENTER_CRITICAL(&touch_mux);
    touch_ring[touch_head % TOUCH_RING] = p;
    touch_head++;
    if (touch_head - touch_tail > TOUCH_RING) touch_tail = touch_head - TOUCH_RING;   // LVGL stalled: the oldest go
    portEXIT_CRITICAL(&touch_mux);
}

// At the lift: drop the samples still waiting at the end that move against the motion of the last ~100 ms
static void touch_unwobble(void)
{
    portENTER_CRITICAL(&touch_mux);
    int n = touch_head - touch_tail;
    if (n >= 3) {
        int first = touch_head - (n < 10 ? n : 10);              // ~100 ms of samples, if still waiting
        for (int i = touch_head - 1; i >= first; i--) if (!touch_ring[i % TOUCH_RING].down) { first = i + 1; break; }   // never across the last lift
        int dx = touch_ring[(touch_head - 1) % TOUCH_RING].x - touch_ring[first % TOUCH_RING].x;
        while (touch_head - touch_tail > 1) {
            int a = touch_ring[(touch_head - 2) % TOUCH_RING].x, b = touch_ring[(touch_head - 1) % TOUCH_RING].x;
            if ((dx > 0 && b < a) || (dx < 0 && b > a)) touch_head--;   // turned back at the lift: not the flick
            else break;
        }
    }
    portEXIT_CRITICAL(&touch_mux);
}

static void touch_task(void *arg)
{
    bool was = false;
    int lx = 0, ly = 0;
    int64_t last_down = 0;
    int misses = 0, hx[2] = {0}, hy[2] = {0}, hn = 0;   // failed reads in a row while down; the last two raw samples
    for (;;) {
        // Every 10 ms while a finger is down and a second after (the flick's samples); every 30 ms otherwise,
        // as 0.9.5 did: the chip sleeps between touches and each idle poll is a timeout on the shared bus the
        // codecs and the expander use (931 failed polls in 41 min at 10 ms, and a codec write lost, 2026-11-12)
        vTaskDelay(pdMS_TO_TICKS(was || esp_timer_get_time() - last_down < 1000000 ? 10 : 30));
        if (healing) continue;
        int x, y, e0, e1, r, h;
        board_i2c_counts(&e0, &r, &h);
        bool down = !sleeping && board_touch_read(&x, &y);
        board_i2c_counts(&e1, &r, &h);
        // A read that FAILED on the busy shared bus is not a lifted finger: one mid-swipe split the swipe into a
        // release and a new press, and the half-swipe snapped the wrong way. Held up to 3 failures (30 ms).
        if (!down && was && e1 != e0 && ++misses <= 3) continue;
        misses = 0;
        if (down) {
            last_down = esp_timer_get_time();
            trace_add(x, y);
            // the median of the last three: one garbled coordinate (a 17 px jump back mid-swipe, 2026-11-12) never wins
            if (!was) hn = 0;
            if (hn >= 2) {
                int mx = x, my = y;
                #define MED3(a, b, c) ((a) > (b) ? ((b) > (c) ? (b) : (a) > (c) ? (c) : (a)) : ((a) > (c) ? (a) : (b) > (c) ? (c) : (b)))
                mx = MED3(hx[0], hx[1], x); my = MED3(hy[0], hy[1], y);
                #undef MED3
                hx[0] = hx[1]; hy[0] = hy[1]; hx[1] = x; hy[1] = y;
                x = mx; y = my;
            } else { hx[hn] = x; hy[hn] = y; hn++; }
            y += settings.flip ? -settings.touch_dy : settings.touch_dy;   // the chip reads a few mm toward one edge; turned 180 that edge is the other one
            y = y < 0 ? 0 : y > BOARD_H - 1 ? BOARD_H - 1 : y;
            lx = x; ly = y;
            touch_push((touch_pt_t){x, y, true});
        } else if (was) {
            trace_add(-1, -1);                     // the release: -1, -1
            touch_unwobble();
            touch_push((touch_pt_t){lx, ly, false});
        }
        was = down;
    }
}

static void on_touch_read(lv_indev_t *dev, lv_indev_data_t *data)
{
    touch_pt_t p;
    bool got = false;
    portENTER_CRITICAL(&touch_mux);
    if (touch_tail < touch_head) { p = touch_ring[touch_tail % TOUCH_RING]; touch_tail++; got = true; }
    bool more = touch_tail < touch_head;
    portEXIT_CRITICAL(&touch_mux);
    if (!got) p = touch_last;                      // nothing new: the finger is where it was (or still up)
    touch_last = p;
    data->continue_reading = more;                 // LVGL asks again at once: every waiting sample, in order
    bool down = p.down;
    int x = p.x, y = p.y;
    static bool logged_down;
    if (down && got && !logged_down && !echo_was) { touch_log[touch_log_at % TOUCH_LOG] = (typeof(touch_log[0])){x, y, esp_timer_get_time()}; touch_log_at++; }   // touch-downs, as /health says
    if (got) logged_down = down;
    if (echo_until_us && esp_timer_get_time() < echo_until_us) {
        if (down && !echo_was) { printf("TOUCH %d %d\n", x, y); fflush(stdout); }
        echo_was = down;
    }
    if (down && (!screen_on || dimmed || asleep)) { wake_screen(); swallow = true; }   // the touch that wakes the glass does nothing else
    if (!down) swallow = false;
    if (down && !swallow) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else data->state = LV_INDEV_STATE_RELEASED;
}

// --- how long the glass takes to draw (bench, 1.0.0): every LVGL refresh timed, start to ready -------
#define REFR_LOG 64
static int64_t refr_t0;
static struct { int64_t at; int ms; } refr_log[REFR_LOG];
static int refr_at, refr_slow;          // refreshes over 50 ms since boot

static void on_refr(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) { refr_t0 = now; return; }
    if (!refr_t0) return;
    int ms = (int)((now - refr_t0) / 1000);
    refr_log[refr_at++ % REFR_LOG].at = now;
    refr_log[(refr_at - 1) % REFR_LOG].ms = ms;
    if (ms > 50) refr_slow++;
}

cJSON *ui_refresh_json(void)
{
    int64_t now = esp_timer_get_time();
    int n = 0, sum = 0, worst = 0, last_s = 0;
    for (int i = 0; i < REFR_LOG; i++) {
        if (!refr_log[i].at || now - refr_log[i].at > 10000000) continue;   // the last 10 s
        n++; sum += refr_log[i].ms;
        if (refr_log[i].ms > worst) worst = refr_log[i].ms;
        if (now - refr_log[i].at < 1000000) last_s++;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "count_10s", n);
    cJSON_AddNumberToObject(j, "mean_ms", n ? sum / n : 0);
    cJSON_AddNumberToObject(j, "worst_ms", worst);
    cJSON_AddNumberToObject(j, "last_1s", last_s);
    cJSON_AddNumberToObject(j, "slow", refr_slow);
    cJSON *recent = cJSON_AddArrayToObject(j, "recent");               // the last 24, oldest first: a slide's frames side by side
    for (int i = REFR_LOG - 24; i < REFR_LOG; i++) {
        int k = (refr_at + i) % REFR_LOG;
        if (refr_log[k].at && now - refr_log[k].at < 10000000) cJSON_AddItemToArray(recent, cJSON_CreateNumber(refr_log[k].ms));
    }
    return j;
}

void ui_start(void)
{
    board_glass_t *g = board_glass();
    if (!g->up) { ESP_LOGW(TAG, "no glass"); return; }
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_priority = 4;
    port.task_stack = 10240;
    port.task_affinity = 1;
    port.task_stack_caps = MALLOC_CAP_SPIRAM;
    port.task_max_sleep_ms = 500;
    port.timer_period_ms = 5;
    if (lvgl_port_init(&port) != ESP_OK) { ESP_LOGE(TAG, "LVGL did not start"); return; }
    const lvgl_port_display_cfg_t dcfg = {
        .io_handle = g->io, .panel_handle = g->panel,
        .buffer_size = BOARD_W * BOARD_DRAW_LINES, .double_buffer = true,
        .hres = BOARD_W, .vres = BOARD_H, .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},   // the panel is turned by board_glass_orient
        .flags = {.buff_dma = true, .buff_spiram = true, .swap_bytes = true},
    };
    disp = lvgl_port_add_disp(&dcfg);
    if (disp) {
        lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_READY, NULL);
    }
    if (!disp) { ESP_LOGE(TAG, "LVGL did not take the panel"); return; }
    lvgl_port_lock(0);
    // The port sends the panel ITS rotation (none) as it takes the display, undoing the turn
    // board_glass_init gave it: a board set upside down came up the right way up at every boot
    // while its touch stayed turned, so taps landed opposite their buttons ("it isn't saving",
    // Krem 2026-11-03: it was saved; the port overwrote the panel). Ours goes last.
    board_glass_orient(settings.invert, settings.flip, settings.mirror);
    lv_display_set_default(disp);
    if (g->touch) {
        indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, on_touch_read);
        task_in_psram(touch_task, "touch", 6144, 5, 1);   // the IDF's I2C error logs run on this stack (through the log ring)   // above LVGL (4): sampled every 10 ms whatever it draws
        lv_indev_set_display(indev, disp);
        lv_indev_set_long_press_time(indev, HOLD_MS);
        lv_timer_set_period(lv_indev_get_read_timer(indev), 10);   // Elecrow reads the touch every 10 ms: the feel
    }
    build();
    beat_us = esp_timer_get_time();
    lvgl_port_unlock();
    ok = true;
    board_backlight(settings.brightness);
    ESP_LOGI(TAG, "glass built, %s", indev ? "touch on" : "NO touch");
}

bool ui_ok(void) { return ok; }

#define DOOR_MS 500
static int doors_shut;

static bool take(void)
{
    if (lvgl_port_lock(DOOR_MS)) return true;
    doors_shut++;
    return false;
}

bool ui_orient(void)
{
    if (!ok) { board_glass_orient(settings.invert, settings.flip, settings.mirror); return true; }
    if (!take()) return false;
    board_glass_orient(settings.invert, settings.flip, settings.mirror);
    lv_obj_invalidate(lv_screen_active());               // the memory was cleared: everything is drawn again, the other way up
    lv_obj_invalidate(lv_layer_top());
    lvgl_port_unlock();
    return true;
}

int ui_beat_age_ms(void) { return ok && !sleeping ? (int)((esp_timer_get_time() - beat_us) / 1000) : -1; }
int ui_doors_shut(void) { return doors_shut; }
int64_t ui_idle_ms(void) { return ok ? (int64_t)lv_display_get_inactive_time(disp) : 0; }

bool ui_show_app(int which)
{
    if (!ok || which < 0 || which >= APPS || !take()) return false;
    lv_tileview_set_tile_by_index(tv, which, 0, LV_ANIM_ON);
    wake_screen();
    lvgl_port_unlock();
    return true;
}

// Dark, and LVGL's lock held until ui_sleep(false) on the same task: nothing paints while the
// panel has no power. Back: the whole glass drawn afresh, as if just touched.
// LVGL's 5 ms tick (an esp_timer) is stopped for the sleep: esp_timer replays every period a
// periodic timer missed in light sleep, so a tick slept eight hours fired 5.76 million times in a
// row at the wake, on core 0 at priority 22, and lv_tick leapt by the whole sleep: the glass came
// back "dim" (inactive longer than dim_after), then idle past sleep_after, and slept again, with
// nothing on core 0 answering meanwhile (Krem's wake: dim, unresponsive, dark in 10 s, 2026-11-05).
void ui_sleep(bool on)
{
    if (!ok) return;
    if (on) {
        link_shown(false);                                // her tile goes dark with it: Sapphire stops her idle now (its tick stops below)
        lvgl_port_lock(0);
        picker_close();
        board_backlight(0);
        sleeping = true;
        screen_on = false;
        lvgl_port_stop();
    } else {
        lvgl_port_resume();
        sleeping = false;
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
        wake_screen();
        beat_us = esp_timer_get_time();
        lvgl_port_unlock();
    }
}

// The expander was found reset and put back (board_speaker_check from the watch task): the rail had
// dropped under the panel, so it forgot its setup; it is brought up again under LVGL's lock, the
// glass repainted whole, the backlight where it was (scouts 2026-11-05).
void ui_heal(void)
{
    if (!ok || !take()) return;
    healing = true;                                      // the touch task keeps off the chip while its reset is pulsed
    if (!board_glass_wake()) ESP_LOGE(TAG, "the glass did not come back after the expander's heal");
    healing = false;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    if (screen_on && !sleeping) board_backlight(asleep ? 0 : dimmed ? settings.dim_percent : settings.brightness);
    lvgl_port_unlock();
}

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
    draw_clock();
    lvgl_port_unlock();
}

uint8_t *ui_image_begin(int kind, int w, int h)
{
    if (!ok || kind < 0 || kind > 1 || w < 1 || h < 1 || w > BOARD_W || h > BOARD_H || !take()) return NULL;
    image_t *im = &images[kind];
    uint8_t *px = NULL;
    uint8_t *fresh = heap_caps_aligned_alloc(64, (size_t)w * h * 2, MALLOC_CAP_SPIRAM);
    if (fresh) {
        if (kind == UI_PICTURE && im->px && !im->filling) {
            free(im->old);
            im->old = im->px;
        } else image_free(kind);
        im->px = px = fresh;
        im->w = w; im->h = h;
        im->filling = true;
    }
    lvgl_port_unlock();
    return px;
}

bool ui_image_show(int kind, int w, int h, int seconds, const char *name)
{
    if (!ok || kind < 0 || kind > 1 || !take()) return false;
    image_t *im = &images[kind];
    bool shown = false;
    if (im->px && im->filling && im->w == w && im->h == h) {
        uint16_t *p = (uint16_t *)im->px;
        for (size_t i = 0, n = (size_t)w * h; i < n; i++) p[i] = (uint16_t)(p[i] << 8 | p[i] >> 8);
        im->filling = false;
        image_apply(kind, LV_COLOR_FORMAT_RGB565);
        free(im->old);
        im->old = NULL;
        if (kind == UI_SCENE) strlcpy(scene_name, name ? name : "", sizeof scene_name);
        else {
            stream_stop();
            picture_timer_arm(seconds);
            if (screen_on) wake_screen();
            picker_close();
        }
        shown = true;
    }
    lvgl_port_unlock();
    return shown;
}

#define FRAME_LINES 16
#define FRAME_GAP_US (1000000LL / UI_STREAM_FPS)
#define GAPS_N 32                      // the last paints' gaps, for /health screen.stream (steady or choppy, as a number)
static int gaps_ms[GAPS_N];
static unsigned gaps_at;

// Her strips are drawn AROUND the overlays' boxes (the clock, the dot, the corners, a toast):
// a button painted over and repainted a tick later flashed at every frame (0.1.34). LVGL then
// repaints each box with the new frame under its overlay, in one flush, and nothing flashes.
#define HOLES_MAX 24
// The panel's transfers are queued (trans_queue_depth 10, board.c) and read their source as
// they go: one scratch buffer reused per segment was overwritten under a transfer still in
// flight, and her eyes scrambled beside the centre dot (0.1.35). A ring of more buffers
// than the queue holds, in PSRAM (the DMA reads it as it reads the frame itself).
#define BLIT_RING 12
#define BLIT_ROWS 48                   // rows a draw: each call waits out the previous transfer before its window commands, so fewer, bigger calls (0.1.39; was FRAME_LINES 16)
static uint16_t *blit_ring[BLIT_RING];        // each BLIT_ROWS x BOARD_W pixels: a run is never wider than the glass (a 320-wide frame overran FACE-wide buffers)
static bool blit_ring_ok(void)        // only the empty slots are filled: a full refill leaked the ones that had landed (scout)
{
    for (int i = 0; i < BLIT_RING; i++) if (!blit_ring[i]) blit_ring[i] = heap_caps_malloc((size_t)BLIT_ROWS * BOARD_W * 2, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < BLIT_RING; i++) if (!blit_ring[i]) return false;
    return true;
}
static int blit_at;

static int holes_collect(lv_area_t *holes, int max, int fx, int fy, int fw, int fh)   // [x1,x2) [y1,y2) in frame coordinates
{
    int n = 0;
    lv_obj_t *layers[2] = {app[APP_AVATAR], lv_layer_top()};
    for (int l = 0; l < 2; l++) {
        for (uint32_t i = 0, c = lv_obj_get_child_count(layers[l]); i < c && n < max; i++) {
            lv_obj_t *o = lv_obj_get_child(layers[l], i);
            if (o == face || o == av_img || lv_obj_is_hidden(o)) continue;
            lv_area_t b;
            lv_obj_get_coords(o, &b);
            if (b.x2 < fx || b.y2 < fy || b.x1 >= fx + fw || b.y1 >= fy + fh) continue;   // not on her face
            holes[n].x1 = b.x1 < fx ? 0 : b.x1 - fx;
            holes[n].x2 = b.x2 >= fx + fw ? fw : b.x2 - fx + 1;
            holes[n].y1 = b.y1 < fy ? 0 : b.y1 - fy;
            holes[n].y2 = b.y2 >= fy + fh ? fh : b.y2 - fy + 1;
            n++;
        }
    }
    return n;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void blit_around(const uint8_t *frame, int w, int h, int fx, int fy)
{
    lv_area_t holes[HOLES_MAX];
    int n = holes_collect(holes, HOLES_MAX, fx, fy, w, h);
    int edges[2 * HOLES_MAX + 2], ne = 0;
    edges[ne++] = 0;
    edges[ne++] = h;
    for (int i = 0; i < n; i++) { edges[ne++] = holes[i].y1; edges[ne++] = holes[i].y2; }
    qsort(edges, ne, sizeof edges[0], cmp_int);
    for (int e = 0; e + 1 < ne; e++) {                        // bands between hole edges: one set of holes spans each
        int y1 = edges[e], y2 = edges[e + 1];
        if (y2 <= y1) continue;
        int xs[HOLES_MAX][2], nx = 0;
        for (int i = 0; i < n; i++)
            if (holes[i].y1 <= y1 && holes[i].y2 >= y2) { xs[nx][0] = holes[i].x1; xs[nx][1] = holes[i].x2; nx++; }
        for (int i = 1; i < nx; i++) {                        // by x1
            int k0 = xs[i][0], k1 = xs[i][1], j = i - 1;
            while (j >= 0 && xs[j][0] > k0) { xs[j + 1][0] = xs[j][0]; xs[j + 1][1] = xs[j][1]; j--; }
            xs[j + 1][0] = k0; xs[j + 1][1] = k1;
        }
        for (int y = y1; y < y2; y += BLIT_ROWS) {
            int rows = y2 - y < BLIT_ROWS ? y2 - y : BLIT_ROWS;
            int x = 0;
            for (int i = 0; i <= nx; i++) {
                int sx2 = i < nx ? xs[i][0] : w;              // the run [x, sx2) is hers
                if (sx2 > x) {
                    int sw = sx2 - x;
                    uint16_t *buf = blit_ring[blit_at++ % BLIT_RING];
                    for (int r = 0; r < rows; r++) memcpy(buf + r * sw, frame + ((size_t)(y + r) * w + x) * 2, (size_t)sw * 2);
                    esp_lcd_panel_draw_bitmap(board_glass()->panel, fx + x, fy + y, fx + sx2, fy + y + rows, buf);
                }
                if (i < nx && xs[i][1] > x) x = xs[i][1];
            }
        }
    }
}

// Paints the frame waiting for the beat. With the home tile in view and still, the panel is
// written straight at the face's place, around the overlays, and LVGL's picture pointed at the
// frame without a redraw; during a swipe, or elsewhere, LVGL paints it the slow way.
static bool paint_next(void)
{
    image_t *im = &images[UI_PICTURE];
    if (!next_px) return false;
    // While a decode fills im->px, the glass shows im->old (ui_image_begin moved it there); the
    // direct paint swaps THAT, and ui_frame_ready hands it back as im->px when the decode ends.
    // Waiting for the decode instead held every paint until the next frame was in: 167 ms of
    // beat plus ~45 ms of decode, 212 ms a frame, 30 frames a reply replaced (0.1.38).
    uint8_t **shown = im->filling ? &im->old : &im->px;
    bool direct = on_avatar_tile() && !lv_obj_is_hidden(face) && *shown && is_stream(im)
                  && im->dsc.header.w == next_w && im->dsc.header.h == next_h;
    if (!direct && im->filling) return false;         // the slow path points LVGL at im->px: not while it is being filled
    uint8_t *frame = next_px;
    int w = next_w, h = next_h;
    next_px = NULL;
    // The beat is start to start: stamped here, before the blit. Stamped after it (until
    // 0.1.46) every period was the beat plus the blit, ~40 ms, in every number of the night:
    // 167 + 40 = 207, 100 + 43 = 143, 200 + 39 = 239.
    int64_t now_us = esp_timer_get_time();
    if (frame_painted_us && now_us - frame_painted_us < 5000000) gaps_ms[gaps_at++ & (GAPS_N - 1)] = (int)((now_us - frame_painted_us) / 1000);   // a stream, not a first frame
    frame_painted_us = now_us;
    if (direct) {
        im->dsc.data = frame;
        free(*shown);
        *shown = frame;
        lv_obj_update_layout(face);
        lv_area_t a;
        lv_obj_get_coords(face, &a);
        if (blit_ring_ok()) blit_around(frame, w, h, a.x1, a.y1);
        lv_obj_t *top = lv_layer_top();
        for (uint32_t i = 0, n = lv_obj_get_child_count(top); i < n; i++) {
            lv_obj_t *o = lv_obj_get_child(top, i);
            if (!lv_obj_is_hidden(o)) lv_obj_invalidate(o);
        }
    } else {
        free(im->px);
        im->px = frame;
        im->w = w; im->h = h;
        image_apply(UI_PICTURE, LV_COLOR_FORMAT_RGB565_SWAPPED);
    }
    frames_painted++;
    if (screen_on) wake_screen();
    return true;
}

void ui_stream_gaps(int *mean_ms, int *max_ms, int *late)
{
    int n = gaps_at < GAPS_N ? gaps_at : GAPS_N, sum = 0, mx = 0, lt = 0;
    for (int i = 0; i < n; i++) { int g = gaps_ms[i]; sum += g; if (g > mx) mx = g; }
    int mean = n ? sum / n : 0;
    for (int i = 0; i < n; i++) if (gaps_ms[i] > mean + mean / 2) lt++;          // late: half again the window's own cadence (idle paints at 2 fps, speech at the beat)
    *mean_ms = mean; *max_ms = mx; *late = lt;
}

bool ui_stream_live(void) { return frame_painted_us && esp_timer_get_time() - frame_painted_us < 30000000; }   // 30 s: a check does not resume between her sentences (Krem, 2026-11-03)

static void on_frame_tick(lv_timer_t *t);
static void paint_later(uint32_t ms)
{
    if (frame_timer) return;
    frame_timer = lv_timer_create(on_frame_tick, ms ? ms : 1, NULL);
    lv_timer_set_repeat_count(frame_timer, 1);
}
static void on_frame_tick(lv_timer_t *t)
{
    frame_timer = NULL;
    if (!paint_next() && next_px) paint_later(20);
}

bool ui_frame_ready(int w, int h, int seconds, bool taps, bool *replaced)
{
    if (replaced) *replaced = false;
    if (!ok || !take()) return false;
    image_t *im = &images[UI_PICTURE];
    bool taken = false;
    if (!stream_hidden && lv_tileview_get_tile_active(tv) != app[APP_AVATAR]) lv_tileview_set_tile_by_index(tv, APP_AVATAR, 0, LV_ANIM_OFF);   // a game or a cast: its picture opens her tile by itself (from a status page too)
    if (im->px && im->filling && im->w == w && im->h == h && stream_hidden) {
        free(im->px);                                    // not to be shown now: dropped, the one before stays
        im->px = im->old;
        im->old = NULL;
        im->filling = false;
        if (im->px) { im->w = im->dsc.header.w; im->h = im->dsc.header.h; }
        lvgl_port_unlock();
        return false;
    }
    if (im->px && im->filling && im->w == w && im->h == h) {
        im->filling = false;
        picker_close();
        stream_taps = taps;
        lv_obj_set_scrollable(tv, !taps);  // ONE fact with the frame taken: a game holds the glass (a swipe is a move), anything else lets it go
        if (next_px) { free(next_px); frames_replaced++; if (replaced) *replaced = true; }
        next_px = im->px;
        next_w = w; next_h = h;
        im->px = im->old;
        im->old = NULL;
        if (im->px) { im->w = im->dsc.header.w; im->h = im->dsc.header.h; }
        picture_timer_arm(seconds);
        int64_t since = esp_timer_get_time() - frame_painted_us;
        if (since >= FRAME_GAP_US) paint_next();
        else paint_later((uint32_t)((FRAME_GAP_US - since) / 1000) + 1);
        taken = true;
    }
    lvgl_port_unlock();
    return taken;
}

void ui_stream_counts(int *painted, int *replaced) { *painted = frames_painted; *replaced = frames_replaced; }

static bool image_drop(int kind, bool mine)
{
    if (!ok || kind < 0 || kind > 1 || !take()) return false;
    if (images[kind].filling && !mine) { lvgl_port_unlock(); return true; }
    image_t *im = &images[kind];
    if (mine && kind == UI_PICTURE && im->filling && im->old) {
        free(im->px);
        im->px = im->old;
        im->old = NULL;
        im->w = im->dsc.header.w; im->h = im->dsc.header.h;
        im->filling = false;
        if (!picture_timer) picture_timer_arm(0);
    } else if (kind == UI_PICTURE) {
        if (mine) im->filling = false;
        picture_end();
        if (!mine) stream_left_until_us = 0;
    } else { image_free(UI_SCENE); scene_name[0] = 0; }
    lvgl_port_unlock();
    return true;
}

bool ui_image_drop(int kind) { return image_drop(kind, false); }
bool ui_image_abort(int kind) { return image_drop(kind, true); }

const char *ui_scene_name(void) { return scene_name; }
bool ui_picture_on(void) { return images[UI_PICTURE].px && !images[UI_PICTURE].filling; }
bool ui_stream_hidden(void) { return stream_hidden; }
// --- her stream (avatar.c) ----------------------------------------------------------------
static void blit_rect(const uint8_t *frame, int stride, int x1, int y1, int x2, int y2, int fx, int fy)   // plain strips, no holes
{
    if (!blit_ring_ok()) return;
    // Her box may overscan the glass (the pendant's 288 rows on 284): only what lies on the panel is sent
    if (fx + x1 < 0) x1 = -fx;
    if (fy + y1 < 0) y1 = -fy;
    if (fx + x2 > BOARD_W) x2 = BOARD_W - fx;
    if (fy + y2 > BOARD_H) y2 = BOARD_H - fy;
    if (x2 <= x1 || y2 <= y1) return;
    int sw = x2 - x1;
    for (int y = y1; y < y2; y += BLIT_ROWS) {
        int rows = y2 - y < BLIT_ROWS ? y2 - y : BLIT_ROWS;
        uint16_t *buf = blit_ring[blit_at++ % BLIT_RING];
        for (int r = 0; r < rows; r++) memcpy(buf + r * sw, frame + ((size_t)(y + r) * stride + x1) * 2, (size_t)sw * 2);
        esp_lcd_panel_draw_bitmap(board_glass()->panel, fx + x1, fy + y, fx + x2, fy + y + rows, buf);
    }
}

bool ui_repaint(void)
{
    if (!ok || !take()) return false;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lvgl_port_unlock();
    return true;
}
// Inside awake hours she lives on a dimmed screen too (Krem, 2026-11-12: "always" means always); outside
// them a dimmed screen goes dark anyway (on_tick), and dark is never shown.
static bool av_shown_now(void) { return ok && screen_on && (!dimmed || light_in_hours()) && !asleep && !sleeping && on_avatar_tile()
                               && lv_obj_is_hidden(face) && lv_obj_is_hidden(shade); }   // a game's table or a still picture over her: she is not what shows
// For the HTTP tasks (/health, a packet's answer), which hold no LVGL lock: the tick's answer, 100 ms old at most.
// Asking LVGL from there walked its animation list while the LVGL task freed from it. No tick while it sleeps.
bool ui_av_shown(void) { return av_shown && !sleeping; }
static const char *av_refusal = "";
const char *ui_av_refusal(void) { return av_refusal; }
// The pixels go into her face first, outside the LVGL lock: a region is never lost to a busy lock or
// to being late (0.6.0; before, either was a patch the board never had until the next key, and the
// sender did not know). `show` false: written, not yet sent to the panel; the next shown frame sends
// everything written since with it.
// --- the listen bar, painted INTO her face (1.0.0) -------------------------------------------------
// While she listens, a blue bar near the bottom of her face: the light only shows from the side, and a board
// lies flat on a desk (Krem, 2026-11-13: "I couldn't tell she was listening"). An LVGL object over her would be
// a hole in every blit, so the bar lives in av_px itself: drawn there, re-drawn over any frame that lands on
// it, and the pixels under it kept, to be put back when she stops listening. No holes, no flicker.
#define EAR_W 72
#define EAR_H 5
#define EAR_X ((BOARD_AV_W - EAR_W) / 2)
#define EAR_Y (BOARD_AV_H - (BOARD_AV_H > BOARD_H ? (BOARD_AV_H - BOARD_H) / 2 : 0) - 12)   // 12 px above the glass's bottom edge
static bool ear_on;
static uint16_t ear_under[EAR_W * EAR_H];

static void ear_paint(void)                 // what is in av_px under the bar is kept, then the bar drawn over it
{
    const uint16_t blue = (uint16_t)(((0x2a6fdb >> 19) & 0x1f) << 11 | ((0x2a6fdb >> 10) & 0x3f) << 5 | ((0x2a6fdb >> 3) & 0x1f));
    uint16_t be = (uint16_t)(blue >> 8 | blue << 8);                 // av_px is RGB565 big-endian
    for (int r = 0; r < EAR_H; r++) {
        uint16_t *row = (uint16_t *)av_px + (size_t)(EAR_Y + r) * BOARD_AV_W + EAR_X;
        for (int c = 0; c < EAR_W; c++) {
            if (row[c] != be) ear_under[r * EAR_W + c] = row[c];   // a pixel already blue is the bar's own: the one under it stays kept
            row[c] = be;
        }
    }
}

static void ear_show(void)                  // the bar's rect to the panel, under the LVGL lock (on_tick holds it)
{
    if (lv_obj_is_hidden(av_img)) { lv_obj_set_hidden(av_img, false); lv_obj_invalidate(av_img); return; }
    if (on_avatar_tile() && lv_obj_is_hidden(face) && !top_shown()) {
        lv_obj_update_layout(av_img);
        lv_area_t a;
        lv_obj_get_coords(av_img, &a);
        blit_rect(av_px, BOARD_AV_W, EAR_X, EAR_Y, EAR_X + EAR_W, EAR_Y + EAR_H, a.x1, a.y1);
    } else lv_obj_invalidate(av_img);
}

static void ear_set(bool on)
{
    if (!av_px || on == ear_on) return;
    if (on) ear_paint();
    else for (int r = 0; r < EAR_H; r++)
        memcpy((uint16_t *)av_px + (size_t)(EAR_Y + r) * BOARD_AV_W + EAR_X, ear_under + r * EAR_W, EAR_W * 2);   // her pixels back
    ear_on = on;
    ear_show();
}

bool ui_av_frame(const uint8_t *rgb, int x, int y, int w, int h, bool key, bool voice, bool show)
{
    if (!ok) { av_refusal = "the UI is not up"; return false; }
    if (!av_px) { av_refusal = "no face buffer"; return false; }
    if (x < 0 || y < 0 || x + w > BOARD_AV_W || y + h > BOARD_AV_H) { av_refusal = "outside the face"; return false; }
    for (int r = 0; r < h; r++) memcpy(av_px + ((size_t)(y + r) * BOARD_AV_W + x) * 2, rgb + (size_t)r * w * 2, (size_t)w * 2);
    if (ear_on && x < EAR_X + EAR_W && x + w > EAR_X && y < EAR_Y + EAR_H && y + h > EAR_Y) ear_paint();   // a frame landed on the bar: her new pixels kept under it, the bar on top
    if (dirty_x2 <= dirty_x1) { dirty_x1 = x; dirty_y1 = y; dirty_x2 = x + w; dirty_y2 = y + h; }
    else {
        if (x < dirty_x1) dirty_x1 = x;
        if (y < dirty_y1) dirty_y1 = y;
        if (x + w > dirty_x2) dirty_x2 = x + w;
        if (y + h > dirty_y2) dirty_y2 = y + h;
    }
    if (!show) return true;
    if (!lvgl_port_lock(100)) return true;                // written: the next frame takes it to the panel
    x = dirty_x1; y = dirty_y1; w = dirty_x2 - dirty_x1; h = dirty_y2 - dirty_y1;
    dirty_x1 = dirty_y1 = dirty_x2 = dirty_y2 = 0;
    if (av_hide) { lv_timer_delete(av_hide); av_hide = NULL; }
    if (lv_obj_is_hidden(av_img)) { lv_obj_set_hidden(av_img, false); lv_obj_invalidate(av_img); }
    else if (on_avatar_tile() && lv_obj_is_hidden(face) && !top_shown()) {   // straight to the panel only with nothing over her
        lv_obj_update_layout(av_img);
        lv_area_t a;
        lv_obj_get_coords(av_img, &a);
        blit_rect(av_px, BOARD_AV_W, x, y, x + w, y + h, a.x1, a.y1);
    } else lv_obj_invalidate(av_img);                    // a toast, a dial, a picture or a game over her: LVGL draws her under them
    if (voice && screen_on) wake_screen();               // her idle lets the screen dim and sleep as set
    lvgl_port_unlock();
    return true;
}

// What was written into her face but not yet sent (a late region at a stream's end): to the panel now.
void ui_av_flush(void)
{
    if (!ok || dirty_x2 <= dirty_x1 || !lvgl_port_lock(100)) return;
    int x = dirty_x1, y = dirty_y1, w = dirty_x2 - dirty_x1, h = dirty_y2 - dirty_y1;
    dirty_x1 = dirty_y1 = dirty_x2 = dirty_y2 = 0;
    if (on_avatar_tile() && lv_obj_is_hidden(face) && !top_shown() && !lv_obj_is_hidden(av_img)) {
        lv_obj_update_layout(av_img);
        lv_area_t a;
        lv_obj_get_coords(av_img, &a);
        blit_rect(av_px, BOARD_AV_W, x, y, x + w, y + h, a.x1, a.y1);
    } else lv_obj_invalidate(av_img);
    lvgl_port_unlock();
}

void ui_av_show(void)
{
    if (!ok || !lvgl_port_lock(200)) return;
    if (av_hide) { lv_timer_delete(av_hide); av_hide = NULL; }
    if (ui_app() != APP_AVATAR) lv_tileview_set_tile_by_index(tv, APP_AVATAR, 0, LV_ANIM_ON);   // her stream opens her tile
    lvgl_port_unlock();
}


// A stream ended: her last frame STAYS. The AVATAR tile is hers alone, so hiding her only left a lit,
// empty glass (a ~20 s grey screen while the hub rebuilt, 2026-11-12); a still face is the honest picture.
void ui_av_end(void) {}

bool ui_stream_hidden_for(bool taps) { return stream_hidden; }   // a cast opens her tile (ui_frame_ready), as her stream does: it was invisible from VOICE (scout)

// The "face" job: hide her (frames answered hidden; the last one goes when its seconds run out) or show her again.
void ui_face_toggle(void)
{
    if (stream_hidden) { stream_hidden = false; stream_hide_until_us = 0; }
    else { stream_hidden = true; stream_hide_until_us = INT64_MAX; }
}
bool ui_stream_left(void)
{
    bool was = stream_left_until_us && esp_timer_get_time() < stream_left_until_us;
    stream_left_until_us = 0;
    return was;
}
bool ui_stream_taps(void) { return stream_taps; }

bool ui_status_open(void)
{
    if (!ok || !take()) return false;
    wake_screen();
    lv_tileview_set_tile_by_index(tv, ui_app(), 1, LV_ANIM_ON);
    lvgl_port_unlock();
    return true;
}

bool ui_screen_power(bool on)
{
    if (!ok || !take()) return false;
    if (on) wake_screen();
    else { screen_on = false; board_backlight(0); picker_close(); }
    lvgl_port_unlock();
    return true;
}

bool ui_screen_on(void) { return screen_on; }

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
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_size(btn, 120, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_event_cb(btn, on_card_back, LV_EVENT_CLICKED, card);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "done");
    lv_obj_center(bl);
    lvgl_port_unlock();
}

void ui_test_stuck(void)
{
    if (!ok) return;
    lvgl_port_lock(0);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
