// pet.c - see pet.h.
#include "pet.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "pet";
#define BODY 150
#define EYE 24
#define PUPIL 11

static lv_obj_t *stage_, *body, *eyes[2], *pupils[2], *mouth, *needs, *says, *name_lbl, *buttons;
static lv_obj_t *bars[3];              // hunger, energy, clean, as thin lines under the name
static lv_timer_t *blink_timer, *breathe_timer, *settle_timer;
static pet_state_t st = {.name = "Bit", .stage = 0, .mood = 0.8f, .hunger = 0.3f, .energy = 0.7f, .clean = 0.9f,
                         .doing = PET_IDLE, .says = "hi"};
static bool shown;
static int breath;                     // frame of the breath
static int look_x, look_y;             // where the pupils point, -5..5
static int bounce;                     // frames of bounce left

#define C_BODY lv_color_hex(0x5a8cdc)
#define C_BODY_TIRED lv_color_hex(0x4a5f88)
#define C_EYE lv_color_hex(0xf4f6f8)
#define C_PUPIL lv_color_hex(0x14181f)
#define C_MOUTH lv_color_hex(0x14181f)
#define C_TEXT lv_color_hex(0xe8ecf1)
#define C_DIM lv_color_hex(0x8a93a0)

// --- drawing the state ----------------------------------------------------------------------

static lv_color_t mix(lv_color_t a, lv_color_t b, float t)
{
    return lv_color_mix(b, a, (uint8_t)(t * 255));
}

static void draw_face(void)
{
    // the mouth: a smile that flattens as mood drops, a frown below 0.3
    int span = 20 + (int)(fabsf(st.mood - 0.5f) * 2 * 120);        // 20..140 degrees
    if (st.mood >= 0.5f) { lv_arc_set_bg_angles(mouth, 90 - span / 2, 90 + span / 2); lv_obj_set_style_transform_rotation(mouth, 0, 0); }
    else { lv_arc_set_bg_angles(mouth, 270 - span / 2, 270 + span / 2); }
    if (st.doing == PET_EATING) lv_arc_set_bg_angles(mouth, 30, 150);
    // the body's tint follows energy
    lv_obj_set_style_bg_color(body, mix(C_BODY_TIRED, C_BODY, st.energy), 0);
    // what it needs
    char need[64] = "";
    if (st.doing == PET_SLEEPING) strcpy(need, "zzz");
    else {
        if (st.hunger > 0.6f) strcat(need, "hungry ");
        if (st.energy < 0.3f) strcat(need, "sleepy ");
        if (st.clean < 0.4f) strcat(need, "grubby ");
        if (!need[0]) strcpy(need, st.mood > 0.7f ? "happy" : st.mood > 0.4f ? "ok" : "sulking");
    }
    lv_label_set_text(needs, need);
    lv_label_set_text_fmt(says, "\"%s\"", st.says);
    lv_label_set_text_fmt(name_lbl, "%s, %s", st.name, st.stage == 0 ? "hatchling" : st.stage == 1 ? "kid" : "grown");
    const float v[3] = {1 - st.hunger, st.energy, st.clean};
    for (int i = 0; i < 3; i++) lv_obj_set_width(bars[i], 10 + (int)(v[i] * 70));
    // eyes closed while asleep
    for (int i = 0; i < 2; i++) lv_obj_set_height(eyes[i], st.doing == PET_SLEEPING ? 3 : EYE);
}

// --- life of its own: breathing, blinking, looking --------------------------------------------

static void place_body(void)
{
    float b = 1 + 0.035f * sinf(breath / 25.0f * 2 * M_PI / 3.2f);   // one breath every 3.2 s
    int size = (int)(BODY * (1 + 0.15f * st.stage) * b);
    int dy = bounce ? -(int)(28 * sinf(bounce / 14.0f * M_PI)) : 0;
    lv_obj_set_size(body, size, size);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 56 + (BODY - size) / 2 + dy);
    for (int i = 0; i < 2; i++) {
        lv_obj_align_to(eyes[i], body, LV_ALIGN_CENTER, (i ? 1 : -1) * size / 5, -size / 8);
        lv_obj_align_to(pupils[i], eyes[i], LV_ALIGN_CENTER, look_x, look_y);
    }
    lv_obj_align_to(mouth, body, LV_ALIGN_CENTER, 0, size / 5);
    lv_obj_align_to(needs, body, LV_ALIGN_OUT_TOP_MID, 0, -6);
}

static void on_breathe(lv_timer_t *t)
{
    breath++;
    if (bounce) bounce--;
    if (look_x || look_y) {            // the gaze drifts back to the middle
        if (breath % 6 == 0) { look_x -= look_x > 0 ? 1 : look_x < 0 ? -1 : 0; look_y -= look_y > 0 ? 1 : look_y < 0 ? -1 : 0; }
    }
    place_body();
}

static void on_unblink(lv_timer_t *t)
{
    if (st.doing != PET_SLEEPING) for (int i = 0; i < 2; i++) lv_obj_set_height(eyes[i], EYE);
    lv_timer_delete(t);
}

static void on_blink(lv_timer_t *t)
{
    if (st.doing != PET_SLEEPING) {
        for (int i = 0; i < 2; i++) lv_obj_set_height(eyes[i], 3);
        lv_timer_t *u = lv_timer_create(on_unblink, 130, NULL);
        lv_timer_set_repeat_count(u, 1);
    }
    lv_timer_set_period(t, 2500 + (rand() % 3500));
}

// the stand-in life: a button's effect wears off after a while
static void on_settle(lv_timer_t *t)
{
    st.doing = PET_IDLE;
    strlcpy(st.says, st.mood > 0.7f ? "that was nice" : "hm", sizeof st.says);
    draw_face();
    settle_timer = NULL;
}

static void settle_in(int ms)
{
    if (settle_timer) lv_timer_delete(settle_timer);
    settle_timer = lv_timer_create(on_settle, ms, NULL);
    lv_timer_set_repeat_count(settle_timer, 1);
}

// --- touch ---------------------------------------------------------------------------------

static void on_page_press(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(body, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    int dx = p.x - cx, dy = p.y - cy;
    float d = sqrtf((float)dx * dx + (float)dy * dy);
    if (d > 1) { look_x = (int)(5 * dx / d); look_y = (int)(5 * dy / d); }
    place_body();
}

static void on_poke(lv_event_t *e)
{
    bounce = 14;
    if (st.doing == PET_SLEEPING) { st.doing = PET_IDLE; strlcpy(st.says, "mm?", sizeof st.says); }
    else strlcpy(st.says, st.mood > 0.6f ? "hehe" : "..", sizeof st.says);
    st.mood = fminf(1, st.mood + 0.02f);
    draw_face();
    ESP_LOGI(TAG, "poked");
}

static void on_button(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    switch (which) {
    case 0: st.doing = PET_EATING; st.hunger = fmaxf(0, st.hunger - 0.3f); st.mood = fminf(1, st.mood + 0.1f); strlcpy(st.says, "nom", sizeof st.says); break;
    case 1: st.doing = PET_PLAYING; st.mood = fminf(1, st.mood + 0.15f); st.energy = fmaxf(0, st.energy - 0.15f); strlcpy(st.says, "wheee", sizeof st.says); bounce = 14; break;
    case 2: st.doing = PET_WASHING; st.clean = 1; strlcpy(st.says, "sparkly", sizeof st.says); break;
    }
    draw_face();
    settle_in(2500);
}

// --- build ---------------------------------------------------------------------------------

static lv_obj_t *circle(lv_obj_t *parent, int size, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void pet_build(lv_obj_t *page)
{
    stage_ = lv_obj_create(page);
    lv_obj_remove_style_all(stage_);
    lv_obj_set_width(stage_, LV_PCT(100));
    lv_obj_set_flex_grow(stage_, 1);
    lv_obj_remove_flag(stage_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(stage_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stage_, on_page_press, LV_EVENT_PRESSING, NULL);

    needs = lv_label_create(stage_);
    lv_obj_set_style_text_font(needs, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(needs, C_DIM, 0);

    body = circle(stage_, BODY, C_BODY);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, on_poke, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(body, on_page_press, LV_EVENT_PRESSING, NULL);
    for (int i = 0; i < 2; i++) {
        eyes[i] = circle(stage_, EYE, C_EYE);
        lv_obj_set_style_radius(eyes[i], EYE / 2, 0);
        pupils[i] = circle(stage_, PUPIL, C_PUPIL);
    }
    mouth = lv_arc_create(stage_);
    lv_obj_remove_style(mouth, NULL, LV_PART_KNOB);
    lv_obj_remove_style(mouth, NULL, LV_PART_INDICATOR);
    lv_obj_set_size(mouth, 44, 44);
    lv_arc_set_rotation(mouth, 0);
    lv_arc_set_bg_angles(mouth, 40, 140);
    lv_obj_set_style_arc_width(mouth, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(mouth, C_MOUTH, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(mouth, true, LV_PART_MAIN);
    lv_obj_remove_flag(mouth, LV_OBJ_FLAG_CLICKABLE);

    name_lbl = lv_label_create(stage_);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(name_lbl, C_TEXT, 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_MID, 0, 56 + BODY + 40);
    const uint32_t hex[3] = {0xe0a040, 0x60c080, 0x60a0e0};
    for (int i = 0; i < 3; i++) {
        bars[i] = lv_obj_create(stage_);
        lv_obj_remove_style_all(bars[i]);
        lv_obj_set_size(bars[i], 60, 5);
        lv_obj_set_style_radius(bars[i], 3, 0);
        lv_obj_set_style_bg_color(bars[i], lv_color_hex(hex[i]), 0);
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, 0);
        lv_obj_align(bars[i], LV_ALIGN_TOP_LEFT, 20 + i * 95, 56 + BODY + 70);
    }
    says = lv_label_create(stage_);
    lv_obj_set_style_text_font(says, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(says, C_DIM, 0);
    lv_obj_align(says, LV_ALIGN_TOP_MID, 0, 56 + BODY + 90);

    buttons = lv_obj_create(page);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_size(buttons, LV_PCT(100), 60);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *labels[3] = {"feed", "play", "clean"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(buttons);
        lv_obj_set_size(b, 88, 44);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, labels[i]);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, on_button, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    breathe_timer = lv_timer_create(on_breathe, 40, NULL);
    blink_timer = lv_timer_create(on_blink, 3000, NULL);
    lv_timer_pause(breathe_timer);
    lv_timer_pause(blink_timer);
    draw_face();
    place_body();
}

void pet_shown(bool on)
{
    shown = on;
    if (!breathe_timer) return;
    if (on) { lv_timer_resume(breathe_timer); lv_timer_resume(blink_timer); place_body(); }
    else { lv_timer_pause(breathe_timer); lv_timer_pause(blink_timer); }
}

void pet_set_state(const pet_state_t *s)
{
    lvgl_port_lock(0);
    st = *s;
    draw_face();
    place_body();
    lvgl_port_unlock();
}
