// lastwords.c - see lastwords.h.
#include "lastwords.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/freertos_debug.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "xtensa_context.h"

static const char *TAG = "lastwords";
#define MAGIC 0x5a99b1e5
#define MAX_TASKS 32
#define DEPTH 10
#define QUICK_S 300                    // a watchdog restart this soon after boot is a strike

typedef struct { char name[12]; int8_t core; uint8_t n, running; uint32_t pc[DEPTH]; } chain_t;
typedef struct {
    uint32_t magic;
    char reason[64], version[12];
    int64_t when;                      // epoch seconds, 0 when the clock was not set
    uint32_t up_s;
    uint16_t n;
    chain_t tasks[MAX_TASKS];
} record_t;

static RTC_NOINIT_ATTR record_t fresh;         // written on the way down, read on the way up
static RTC_NOINIT_ATTR uint32_t strikes_magic, strikes;
static record_t *kept;                         // the last one, from flash; PSRAM
static esp_reset_reason_t reset;

// Every task's call chain. A task running on the other core this instant is
// marked: its saved frame is from its last switch, so its chain is old.
static void capture(record_t *r)
{
    static TaskSnapshot_t snaps[MAX_TASKS];
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    UBaseType_t n = uxTaskGetSnapshotAll(snaps, MAX_TASKS, NULL);
    r->n = 0;
    for (UBaseType_t i = 0; i < n && r->n < MAX_TASKS; i++) {
        TaskHandle_t h = (TaskHandle_t)snaps[i].pxTCB;
        chain_t *c = &r->tasks[r->n++];
        memset(c, 0, sizeof *c);
        strlcpy(c->name, pcTaskGetName(h), sizeof c->name);
        BaseType_t core = xTaskGetCoreID(h);
        c->core = core == tskNO_AFFINITY ? -1 : (int8_t)core;
        esp_backtrace_frame_t f = {0};
        if (h == me) esp_backtrace_get_start(&f.pc, &f.sp, &f.next_pc);
        else {
            for (int k = 0; k < configNUMBER_OF_CORES; k++) if (xTaskGetCurrentTaskHandleForCore(k) == h) c->running = 1;
            XtExcFrame *x = (XtExcFrame *)snaps[i].pxTopOfStack;
            f.pc = x->pc; f.sp = x->a1; f.next_pc = x->a0;
        }
        c->pc[c->n++] = esp_cpu_process_stack_pc(f.pc);
        while (c->n < DEPTH && f.next_pc != 0 && esp_backtrace_get_next_frame(&f)) c->pc[c->n++] = esp_cpu_process_stack_pc(f.pc);
    }
}

static void stamp(record_t *r, const char *reason)
{
    r->magic = MAGIC;
    strlcpy(r->reason, reason, sizeof r->reason);
    strlcpy(r->version, PROJECT_VER, sizeof r->version);
    time_t now = time(NULL);
    r->when = now > 1600000000 ? now : 0;
    r->up_s = esp_timer_get_time() / 1000000;
}

void lastwords_die(const char *reason)
{
    ESP_LOGE(TAG, "%s: recording every task and restarting", reason);
    stamp(&fresh, reason);
    capture(&fresh);
    if (strikes_magic != MAGIC) { strikes_magic = MAGIC; strikes = 0; }
    strikes = fresh.up_s < QUICK_S ? strikes + 1 : 1;
    esp_restart();
    for (;;) { }
}

static const char *reset_words(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "reset pin";
    default: return "unknown";
    }
}

const char *lastwords_reset(void) { return reset_words(reset); }
int lastwords_strikes(void) { return strikes_magic == MAGIC ? (int)strikes : 0; }

static void keep(const record_t *r)
{
    nvs_handle_t h;
    if (nvs_open("lastwords", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "record", r, sizeof *r);
    nvs_commit(h);
    nvs_close(h);
}

// A panic left a core dump: its summary (the task that fell, where, its chain) becomes the record.
static bool from_core_dump(record_t *r)
{
    if (esp_core_dump_image_check() != ESP_OK) return false;
    esp_core_dump_summary_t *s = calloc(1, sizeof *s);
    bool ok = s && esp_core_dump_get_summary(s) == ESP_OK;
    if (ok) {
        memset(r, 0, sizeof *r);
        r->magic = MAGIC;
        snprintf(r->reason, sizeof r->reason, "crash in %s (%s)", s->exc_task, reset_words(reset));
        strlcpy(r->version, PROJECT_VER, sizeof r->version);   // the dump is this program's unless an update came between
        chain_t *c = &r->tasks[0];
        strlcpy(c->name, s->exc_task, sizeof c->name);
        c->core = -1;
        c->pc[c->n++] = s->exc_pc;
        for (uint32_t i = 0; i < s->exc_bt_info.depth && c->n < DEPTH; i++) c->pc[c->n++] = s->exc_bt_info.bt[i];
        r->n = 1;
    }
    free(s);
    esp_core_dump_image_erase();                               // told once
    return ok;
}

void lastwords_start(void)
{
    reset = esp_reset_reason();
    if (reset == ESP_RST_POWERON || strikes_magic != MAGIC) { strikes_magic = MAGIC; strikes = 0; }
    kept = heap_caps_calloc(1, sizeof *kept, MALLOC_CAP_SPIRAM);
    if (!kept) return;
    // our own watchdog spoke on the way down. Not only after a plain restart: a restart that hangs is
    // ended by the chip's own watchdog (seen once, 2026-10-09), and the record is still good then
    bool ours = fresh.magic == MAGIC && reset != ESP_RST_POWERON;
    if (!ours) strikes = 0;                                    // any other boot gives everything a new try
    if (ours) {
        *kept = fresh;
        keep(kept);
    } else if (from_core_dump(kept)) {
        keep(kept);
    } else {
        nvs_handle_t h;
        size_t size = sizeof *kept;
        bool have = nvs_open("lastwords", NVS_READONLY, &h) == ESP_OK;
        if (have) { have = nvs_get_blob(h, "record", kept, &size) == ESP_OK && size == sizeof *kept && kept->magic == MAGIC; nvs_close(h); }
        if (!have) { free(kept); kept = NULL; }
    }
    fresh.magic = 0;
    ESP_LOGI(TAG, "this boot: %s%s%s", reset_words(reset), kept ? "; last words: " : "", kept ? kept->reason : "");
}

static cJSON *chains_json(const record_t *r)
{
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < r->n && i < MAX_TASKS; i++) {
        const chain_t *c = &r->tasks[i];
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "task", c->name);
        cJSON_AddNumberToObject(t, "core", c->core);
        if (c->running) cJSON_AddBoolToObject(t, "running", true);
        char text[DEPTH * 11 + 1] = {0};
        for (int k = 0; k < c->n && k < DEPTH; k++) snprintf(text + strlen(text), sizeof text - strlen(text), "%s0x%08x", k ? " " : "", (unsigned)c->pc[k]);
        cJSON_AddStringToObject(t, "at", text);
        cJSON_AddItemToArray(list, t);
    }
    return list;
}

cJSON *lastwords_json(bool chains)
{
    if (!kept) return NULL;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "reason", kept->reason);
    cJSON_AddStringToObject(j, "firmware", kept->version);
    if (kept->when) cJSON_AddNumberToObject(j, "when", (double)kept->when);
    if (kept->up_s) cJSON_AddNumberToObject(j, "after_s", kept->up_s);
    if (chains) cJSON_AddItemToObject(j, "tasks", chains_json(kept));
    return j;
}

void lastwords_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("lastwords", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
    record_t *was = kept;
    kept = NULL;
    free(was);
}

cJSON *lastwords_where(void)
{
    record_t *r = heap_caps_calloc(1, sizeof *r, MALLOC_CAP_SPIRAM);
    cJSON *j = cJSON_CreateObject();
    if (!r) return j;
    capture(r);
    cJSON_AddNumberToObject(j, "count", uxTaskGetNumberOfTasks());
    cJSON_AddItemToObject(j, "tasks", chains_json(r));
    free(r);
    return j;
}
