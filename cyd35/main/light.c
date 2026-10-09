// light.c - see light.h. 25 frames a second on its own small task.
#include "light.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "board.h"

static const char *TAG = "light";
#define FPS 25
#define ERROR_FRAMES 36                // three red blinks

static const struct { const char *name; uint8_t r, g, b; } COLORS[] = {
    {"sapphire", 90, 140, 220}, {"red", 255, 0, 0}, {"green", 0, 255, 0}, {"blue", 0, 0, 255},
    {"cyan", 0, 255, 255}, {"magenta", 255, 0, 255}, {"yellow", 255, 200, 0}, {"orange", 255, 100, 0},
    {"amber", 255, 140, 0}, {"pink", 255, 80, 140}, {"purple", 140, 0, 255}, {"teal", 0, 160, 140},
    {"lime", 160, 255, 0}, {"white", 255, 255, 255}, {"warm", 255, 180, 110}, {"off", 0, 0, 0},
};
static const char *PATTERNS[PAT_COUNT] = {"solid", "pulse", "blink", "heartbeat", "spin", "rainbow"};
static const char *PATTERN_HELP[PAT_COUNT] = {
    "no motion", "slowly brighter and dimmer", "on and off", "two quick beats, then rest",
    "quick pulses, the color drifting", "colors going round",
};

static bool running;
static SemaphoreHandle_t lock;
static look_t looks[LOOK_COUNT];
static int hours_from = 8 * 60, hours_until = 0;   // minutes of the day; equal = always on
static look_t held;
static int64_t held_until_us;                      // 0 = not held; -1 = until cleared
static bool dark;
static char cue[16] = "starting";
static int error_left;
static int frame;

const look_t LIGHT_PLAIN = {90, 140, 220, false, PAT_SOLID, 60, 0.02f, 0.5f, true};

static const look_t DEFAULTS[LOOK_COUNT] = {
    {90, 140, 220, false, PAT_HEARTBEAT, 33, 0.0f, 0.10f, true},      // resting
    {255, 200, 0, false, PAT_SPIN, 60, 0.02f, 0.45f, true},           // listening (unused: no mic)
    {0, 0, 0, true, PAT_SPIN, 60, 0.02f, 0.45f, true},                // thinking: rainbow spin
    {140, 0, 255, false, PAT_PULSE, 40, 0.02f, 0.45f, true},          // tool: purple pulse
    {0, 255, 255, false, PAT_SOLID, 30, 0.02f, 0.35f, true},          // speaking: cyan
    {255, 0, 0, false, PAT_PULSE, 30, 0.0f, 0.30f, true},             // no link
    {0, 0, 0, false, PAT_PULSE, 20, 0.0f, 0.10f, true},               // night: off
};

const char *LIGHT_SLOTS[LOOK_COUNT] = {"resting", "listening", "thinking", "tool", "speaking", "nolink", "night"};

static bool black(const look_t *l) { return !l || !l->set || (!l->rainbow && !l->r && !l->g && !l->b); }

static void hsv(float h, float level, uint8_t *r, uint8_t *g, uint8_t *b)
{
    h = h - floorf(h);
    float x = h * 6, f = x - floorf(x);
    int i = (int)x % 6;
    float p = 0, q = 1 - f, t = f;
    float rr = i == 0 ? 1 : i == 1 ? q : i == 2 ? p : i == 3 ? p : i == 4 ? t : 1;
    float gg = i == 0 ? t : i == 1 ? 1 : i == 2 ? 1 : i == 3 ? q : i == 4 ? p : p;
    float bb = i == 0 ? p : i == 1 ? p : i == 2 ? t : i == 3 ? 1 : i == 4 ? 1 : q;
    *r = (uint8_t)(rr * level * 255); *g = (uint8_t)(gg * level * 255); *b = (uint8_t)(bb * level * 255);
}

static const char *color_name(const look_t *l)
{
    if (l->rainbow) return "rainbow";
    for (size_t i = 0; i < sizeof COLORS / sizeof COLORS[0]; i++)
        if (COLORS[i].r == l->r && COLORS[i].g == l->g && COLORS[i].b == l->b) return COLORS[i].name;
    return NULL;
}

static float wave(float t, int bpm, pattern_t p)
{
    float beat = t * bpm / 60.0f;
    float ph = beat - floorf(beat);
    switch (p) {
    case PAT_PULSE: return 0.5f + 0.5f * sinf(2 * M_PI * beat);
    case PAT_BLINK: return ph < 0.5f ? 1 : 0;
    case PAT_HEARTBEAT: return ph < 0.12f ? 1 : ph < 0.25f ? 0.15f : ph < 0.37f ? 0.8f : 0;
    case PAT_SPIN: return ph < 0.3f ? 1 - ph / 0.3f * 0.8f : 0.2f * (1 - (ph - 0.3f) / 0.7f);   // a comet going by
    default: return 1;
    }
}

static void draw_look(const look_t *l, float t)
{
    float level, hue;
    if (l->pattern == PAT_RAINBOW) { level = 1; hue = t * l->bpm / 60.0f / 4; }
    else { level = wave(t, l->bpm, l->pattern); hue = t * l->bpm / 60.0f / 8; }
    level = l->floor + (l->ceiling - l->floor) * level;
    level = level * level;             // one bare LED facing you: a square keeps the low end soft
    uint8_t r, g, b;
    if (l->rainbow || l->pattern == PAT_RAINBOW) hsv(hue, level, &r, &g, &b);
    else { r = l->r * level; g = l->g * level; b = l->b * level; }
    board_led(r, g, b);
}

static const look_t *showing(void)
{
    static look_t plain;
    if (!strcmp(cue, "starting")) { plain = (look_t){40, 40, 40, false, PAT_SOLID, 30, 0.05f, 0.05f, true}; return &plain; }
    if (!strcmp(cue, "listening")) return &looks[LOOK_LISTENING];
    if (!strcmp(cue, "thinking")) return &looks[LOOK_THINKING];
    if (!strcmp(cue, "tool")) return &looks[LOOK_TOOL];
    if (!strcmp(cue, "speaking")) return &looks[LOOK_SPEAKING];
    if (!strcmp(cue, "error")) { plain = (look_t){255, 0, 0, false, PAT_BLINK, 150, 0, 0.4f, true}; return &plain; }
    if (!strcmp(cue, "nolink") && light_in_hours()) return &looks[LOOK_NOLINK];
    if (held_until_us) {
        if (held_until_us > 0 && esp_timer_get_time() > held_until_us) held_until_us = 0;
        else return &held;
    }
    if (dark) return NULL;
    return &looks[light_in_hours() ? LOOK_RESTING : LOOK_NIGHT];
}

static void light_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        if (strcmp(cue, "shown")) {
            const look_t *l = showing();
            if (black(l)) board_led(0, 0, 0);
            else draw_look(l, frame / (float)FPS);
        }
        if (error_left && --error_left == 0) strcpy(cue, "idle");
        xSemaphoreGive(lock);
        frame++;
        vTaskDelay(pdMS_TO_TICKS(1000 / FPS));
    }
}

void light_cue(const char *state)
{
    if (!running) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    error_left = strcmp(state, "error") ? 0 : ERROR_FRAMES;
    strlcpy(cue, state, sizeof cue);
    xSemaphoreGive(lock);
}

void light_cue_from_brain(const char *state)
{
    if (!strcmp(state, "thinking") || !strcmp(state, "tool") || !strcmp(state, "idle") || !strcmp(state, "error"))
        light_cue(state);
}

void light_show(int r, int g, int b, int seconds)
{
    if (!running) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    strcpy(cue, "shown");
    board_led(r, g, b);
    xSemaphoreGive(lock);
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    light_cue("idle");
}

static bool number(const cJSON *j, const char *name, float *out)
{
    const cJSON *v = cJSON_GetObjectItem(j, name);
    if (!cJSON_IsNumber(v)) return false;
    *out = (float)v->valuedouble;
    return true;
}

bool light_parse(const cJSON *json, look_t *out, const look_t *base)
{
    look_t l = base ? *base : looks[LOOK_RESTING];
    l.set = true;
    const cJSON *color = cJSON_GetObjectItem(json, "color");
    if (cJSON_IsString(color)) {
        const char *name = color->valuestring;
        if (!strcmp(name, "rainbow")) l.rainbow = true;
        else {
            bool known = false;
            for (size_t i = 0; i < sizeof COLORS / sizeof COLORS[0]; i++)
                if (!strcmp(COLORS[i].name, name)) { l.r = COLORS[i].r; l.g = COLORS[i].g; l.b = COLORS[i].b; known = true; }
            if (!known) return false;
            l.rainbow = false;
        }
    }
    float r, g, b;
    if (number(json, "r", &r) && number(json, "g", &g) && number(json, "b", &b)) {
        l.r = r; l.g = g; l.b = b; l.rainbow = false;
    }
    const cJSON *anim = cJSON_GetObjectItem(json, "animation");
    if (cJSON_IsString(anim)) {
        bool known = false;
        for (int i = 0; i < PAT_COUNT; i++)
            if (!strcmp(PATTERNS[i], anim->valuestring)) { l.pattern = i; known = true; }
        if (!known) return false;
    }
    float v;
    if (number(json, "bpm", &v)) l.bpm = v < 10 ? 10 : v > 120 ? 120 : (int)v;
    if (number(json, "floor", &v)) l.floor = v < 0 ? 0 : v > 1 ? 1 : v;
    if (number(json, "ceiling", &v)) l.ceiling = v < 0 ? 0 : v > 1 ? 1 : v;
    if (l.ceiling < l.floor) l.ceiling = l.floor;
    const cJSON *speed = cJSON_GetObjectItem(json, "speed");
    if (cJSON_IsString(speed)) l.bpm = !strcmp(speed->valuestring, "slow") ? 20 : !strcmp(speed->valuestring, "fast") ? 100 : 60;
    *out = l;
    return true;
}

void light_hold(const look_t *look, float seconds)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    held = *look;
    dark = false;
    held_until_us = seconds > 0 ? esp_timer_get_time() + (int64_t)(seconds * 1000000) : -1;
    xSemaphoreGive(lock);
}

void light_dark(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    dark = true;
    held_until_us = 0;
    xSemaphoreGive(lock);
}

void light_release(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    dark = false;
    held_until_us = 0;
    xSemaphoreGive(lock);
}

void light_set_look(look_slot_t slot, const look_t *look)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    looks[slot] = *look;
    xSemaphoreGive(lock);
}

const look_t *light_look(look_slot_t slot) { return &looks[slot]; }

void light_set_hours(int from_min, int until_min)
{
    hours_from = from_min;
    hours_until = until_min;
}

bool light_in_hours(void)
{
    if (hours_from == hours_until) return true;
    time_t now = time(NULL);
    if (now < 1600000000) return true;
    struct tm t;
    localtime_r(&now, &t);
    int minute = t.tm_hour * 60 + t.tm_min;
    return hours_from < hours_until ? (minute >= hours_from && minute < hours_until)
                                    : (minute >= hours_from || minute < hours_until);
}

cJSON *light_json(const look_t *l)
{
    cJSON *j = cJSON_CreateObject();
    const char *name = color_name(l);
    if (name) cJSON_AddStringToObject(j, "color", name);
    else {
        cJSON *c = cJSON_AddArrayToObject(j, "color");
        cJSON_AddItemToArray(c, cJSON_CreateNumber(l->r));
        cJSON_AddItemToArray(c, cJSON_CreateNumber(l->g));
        cJSON_AddItemToArray(c, cJSON_CreateNumber(l->b));
    }
    cJSON_AddStringToObject(j, "animation", PATTERNS[l->pattern]);
    cJSON_AddNumberToObject(j, "bpm", l->bpm);
    cJSON_AddNumberToObject(j, "floor", (int)(l->floor * 100 + 0.5f) / 100.0);
    cJSON_AddNumberToObject(j, "ceiling", (int)(l->ceiling * 100 + 0.5f) / 100.0);
    return j;
}

cJSON *light_status(void)
{
    cJSON *j = cJSON_CreateObject();
    const look_t *l = showing();
    cJSON_AddStringToObject(j, "state", held_until_us ? "custom" : !strcmp(cue, "idle") ? "idle" : cue);
    cJSON_AddStringToObject(j, "animation", l && l->set ? PATTERNS[l->pattern] : "off");
    cJSON_AddBoolToObject(j, "blackout", black(l));
    cJSON_AddBoolToObject(j, "in_hours", light_in_hours());
    return j;
}

cJSON *light_spec(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *colors = cJSON_AddArrayToObject(j, "colors");
    for (size_t i = 0; i < sizeof COLORS / sizeof COLORS[0]; i++) cJSON_AddItemToArray(colors, cJSON_CreateString(COLORS[i].name));
    cJSON_AddItemToArray(colors, cJSON_CreateString("rainbow"));
    cJSON *anims = cJSON_AddObjectToObject(j, "animations");
    for (int i = 0; i < PAT_COUNT; i++) cJSON_AddStringToObject(anims, PATTERNS[i], PATTERN_HELP[i]);
    cJSON_AddItemToObject(j, "baseline_now", light_json(&looks[LOOK_RESTING]));
    cJSON_AddBoolToObject(j, "is_night", !light_in_hours());
    char hours[32];
    snprintf(hours, sizeof hours, "%02d:%02d-%02d:%02d", hours_from / 60, hours_from % 60, hours_until / 60, hours_until % 60);
    cJSON_AddStringToObject(j, "hours", hours);
    return j;
}

bool light_save(void)
{
    cJSON *j = cJSON_CreateObject();
    for (int i = 0; i < LOOK_COUNT; i++) cJSON_AddItemToObject(j, LIGHT_SLOTS[i], light_json(&looks[i]));
    cJSON_AddNumberToObject(j, "from", hours_from);
    cJSON_AddNumberToObject(j, "until", hours_until);
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    nvs_handle_t h;
    bool ok = text && nvs_open("pocket", NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_str(h, "looks", text) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    cJSON_free(text);
    return ok;
}

void light_load(void)
{
    memcpy(looks, DEFAULTS, sizeof looks);
    nvs_handle_t h;
    if (nvs_open("pocket", NVS_READONLY, &h) != ESP_OK) return;
    size_t size = 0;
    if (nvs_get_str(h, "looks", NULL, &size) == ESP_OK && size < 4000) {
        char *text = malloc(size);
        if (text && nvs_get_str(h, "looks", text, &size) == ESP_OK) {
            cJSON *j = cJSON_Parse(text);
            if (j) {
                for (int i = 0; i < LOOK_COUNT; i++) {
                    const cJSON *one = cJSON_GetObjectItem(j, LIGHT_SLOTS[i]);
                    look_t l;
                    if (one && light_parse(one, &l, &DEFAULTS[i])) looks[i] = l;
                }
                float f, u;
                if (number(j, "from", &f) && number(j, "until", &u)) { hours_from = (int)f; hours_until = (int)u; }
                cJSON_Delete(j);
            }
        }
        free(text);
    }
    nvs_close(h);
}

void light_start(void)
{
    lock = xSemaphoreCreateMutex();
    light_load();
    running = true;
    xTaskCreatePinnedToCore(light_task, "light", 3072, NULL, 6, NULL, 1);
    ESP_LOGI(TAG, "one pixel, ready");
}
