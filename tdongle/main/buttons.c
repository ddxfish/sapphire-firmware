// buttons.c - see buttons.h.
#include "buttons.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "link.h"

static const char *TAG = "buttons";
#define BUTTONS_MAX 8
#define JOB_NAME BUTTON_JOB
#define SAMPLE_MS 5                    // the pins are read this often, by a timer of their own
#define SETTLE_MS 15                   // a level counts once it has held this long
#define LONG_MS 800
#define DOUBLE_MS 300
static const char *WAYS[PRESS_WAYS] = {"short", "long", "double"};

typedef struct { bool raw, down; int64_t raw_ms; } pin_t;   // the pin now, and the level that settled
typedef struct { uint8_t b; bool down, tap; int64_t ms; } edge_t; // a settled change, and when the pin really moved; or one tap
typedef struct {
    bool down;
    int64_t down_ms, up_ms;
    bool held;                         // its long press has fired
    int taps;                          // short presses waiting for a second one
} state_t;

static const button_def_t *defs;
static const button_job_t *jobs;
static int count, job_count;
static pin_t pins[BUTTONS_MAX];           // the timer's
static state_t state[BUTTONS_MAX];        // buttons_poll's
static QueueHandle_t edges;
static char bound[BUTTONS_MAX][PRESS_WAYS][JOB_NAME];   // "" = left to the board
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// One reading of a button: a level that has held SETTLE_MS becomes an edge.
static void seen(int b, bool raw, int64_t now)
{
    pin_t *p = &pins[b];
    if (raw != p->raw) { p->raw = raw; p->raw_ms = now; }
    if (p->raw != p->down && now - p->raw_ms >= SETTLE_MS) {
        p->down = p->raw;
        edge_t e = {b, p->down, false, p->raw_ms};
        xQueueSend(edges, &e, 0);
    }
}

// Reads the pins on its own clock, so a quick press is never missed however
// late buttons_poll runs: a late poll only delays the job.
static void sample(void *arg)
{
    int64_t now = esp_timer_get_time() / 1000;
    for (int b = 0; b < count; b++)
        if (defs[b].gpio >= 0) seen(b, gpio_get_level(defs[b].gpio) == 0, now);
}

void buttons_feed(int b, bool down)
{
    if (b >= 0 && b < count && defs[b].gpio == BUTTON_FED) seen(b, down, esp_timer_get_time() / 1000);
}

void buttons_tap(const char *name)
{
    for (int b = 0; b < count; b++)
        if (defs[b].gpio == BUTTON_TAP && !strcmp(defs[b].name, name)) {
            edge_t e = {b, false, true, 0};
            xQueueSend(edges, &e, 0);
            return;
        }
}

void buttons_setup(const button_def_t *list, int n, const button_job_t *can, int can_count)
{
    defs = list;
    count = n < BUTTONS_MAX ? n : BUTTONS_MAX;
    jobs = can;
    job_count = can_count;
    for (int i = 0; i < count; i++) {
        if (defs[i].gpio < 0) continue;
        gpio_config_t pin = {.pin_bit_mask = 1ULL << defs[i].gpio, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
        gpio_config(&pin);
    }
    edges = xQueueCreate(16, sizeof(edge_t));
    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = sample, .name = "buttons"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, SAMPLE_MS * 1000));
}

// the job this press does now: what Sapphire set, else the board's own. false = none
static bool job_for(int b, int way, char *out)
{
    taskENTER_CRITICAL(&mux);
    strlcpy(out, bound[b][way], JOB_NAME);
    taskEXIT_CRITICAL(&mux);
    if (!out[0] && defs[b].own[way]) strlcpy(out, defs[b].own[way], JOB_NAME);
    return out[0] != 0;
}

bool buttons_does(const char *name, int way, char *job, size_t size)
{
    char out[JOB_NAME] = "";
    for (int b = 0; b < count; b++)
        if (!strcmp(defs[b].name, name)) { job_for(b, way, out); break; }
    if (!strcmp(out, "none")) out[0] = 0;
    strlcpy(job, out, size);
    return out[0] != 0;
}

static void fire(int b, int way)
{
    char job[JOB_NAME];
    if (!job_for(b, way, job)) return;
    ESP_LOGI(TAG, "%s %s: %s", defs[b].name, WAYS[way], job);
    if (!strcmp(job, "tell")) { link_press(defs[b].name, WAYS[way]); return; }
    for (int i = 0; i < job_count; i++)
        if (!strcmp(job, jobs[i].name)) { jobs[i].run(); return; }
}

// Times come from the pin, not from when the poll got to it.
static void edge(const edge_t *e)
{
    int b = e->b;
    state_t *s = &state[b];
    s->down = e->down;
    if (e->down) {
        if (s->taps == 1 && e->ms - s->up_ms >= DOUBLE_MS) { s->taps = 0; fire(b, PRESS_SHORT); }   // too late to be its second
        s->down_ms = e->ms;
        s->held = false;
    } else if (!s->held) {
        char job[JOB_NAME];
        if (e->ms - s->down_ms >= LONG_MS) { s->taps = 0; fire(b, PRESS_LONG); return; }
        s->up_ms = e->ms;
        if (++s->taps == 2) { s->taps = 0; fire(b, PRESS_DOUBLE); }
        else if (!job_for(b, PRESS_DOUBLE, job)) { s->taps = 0; fire(b, PRESS_SHORT); }   // nothing to wait for
    }
}

void buttons_poll(void)
{
    edge_t e;
    while (xQueueReceive(edges, &e, 0)) {
        if (e.tap) fire(e.b, PRESS_SHORT);
        else edge(&e);
    }
    int64_t now = esp_timer_get_time() / 1000;
    for (int b = 0; b < count; b++) {
        state_t *s = &state[b];
        if (s->down && !s->held && now - s->down_ms >= LONG_MS) { s->held = true; s->taps = 0; fire(b, PRESS_LONG); }
        if (!s->down && s->taps == 1 && now - s->up_ms >= DOUBLE_MS) { s->taps = 0; fire(b, PRESS_SHORT); }
    }
}

void buttons_bound(const cJSON *said)
{
    char fresh[BUTTONS_MAX][PRESS_WAYS][JOB_NAME] = {0};
    int set = 0;
    for (int b = 0; b < count; b++) {
        const cJSON *one = cJSON_IsObject(said) ? cJSON_GetObjectItem(said, defs[b].name) : NULL;
        for (int w = 0; w < PRESS_WAYS; w++) {
            const cJSON *job = one ? cJSON_GetObjectItem(one, WAYS[w]) : NULL;
            if (cJSON_IsString(job) && job->valuestring[0]) { strlcpy(fresh[b][w], job->valuestring, JOB_NAME); set++; }
        }
    }
    taskENTER_CRITICAL(&mux);
    memcpy(bound, fresh, sizeof bound);
    taskEXIT_CRITICAL(&mux);
    ESP_LOGI(TAG, "Sapphire set %d press(es)", set);
}

cJSON *buttons_json(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(j, "list");
    for (int b = 0; b < count; b++) {
        cJSON *one = cJSON_CreateObject();
        cJSON_AddStringToObject(one, "name", defs[b].name);
        for (int w = 0; w < PRESS_WAYS; w++)
            if (defs[b].own[w]) cJSON_AddStringToObject(one, WAYS[w], defs[b].own[w]);
        if (defs[b].gpio == BUTTON_TAP) cJSON_AddItemToObject(one, "ways", cJSON_CreateStringArray(WAYS, 1));   // a tap is a short press, and nothing else
        cJSON_AddItemToArray(list, one);
    }
    cJSON *can = cJSON_AddObjectToObject(j, "can");
    for (int i = 0; i < job_count; i++) cJSON_AddStringToObject(can, jobs[i].name, jobs[i].words);
    return j;
}
