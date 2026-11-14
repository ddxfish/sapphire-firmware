// stats.c - see stats.h. Two snapshots of uxTaskGetSystemState, a delta per
// task: unsigned arithmetic, so the 32 bit counter's wrap (every 71 minutes
// on the esp_timer clock) costs one sample, not a wrong one.
#include "stats.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio.h"                     // task_in_psram

static const char *TAG = "stats";
#define SAMPLE_MS 2000
#define MAX_TASKS 48

typedef struct { TaskHandle_t h; uint32_t run; } mark_t;
typedef struct { char name[configMAX_TASK_NAME_LEN]; int core, prio; float pct; unsigned stack; } row_t;

static SemaphoreHandle_t lock;
static TaskStatus_t *now;              // PSRAM
static mark_t prev[MAX_TASKS];
static size_t prev_n;
static uint32_t prev_total;
static row_t rows[MAX_TASKS];
static size_t rows_n;
static float idle[2] = {-1, -1};
static TickType_t sampled;             // when: a sampler that is starved shows as an old sample, not as a calm board

static void sample(void)
{
    uint32_t total = 0;
    size_t n = uxTaskGetSystemState(now, MAX_TASKS, &total);
    if (!n) return;
    uint32_t span = total - prev_total;
    if (prev_n && span) {
        xSemaphoreTake(lock, portMAX_DELAY);
        rows_n = 0;
        float id[2] = {0, 0};
        for (size_t i = 0; i < n; i++) {
            const mark_t *was = NULL;
            for (size_t k = 0; k < prev_n; k++) if (prev[k].h == now[i].xHandle) { was = &prev[k]; break; }
            if (!was) continue;                            // born since the last sample: next time
            row_t *r = &rows[rows_n++];
            strlcpy(r->name, now[i].pcTaskName, sizeof r->name);
            r->core = now[i].xCoreID == tskNO_AFFINITY ? -1 : (int)now[i].xCoreID;
            r->prio = now[i].uxCurrentPriority;
            r->pct = (now[i].ulRunTimeCounter - was->run) * 100.0f / span;   // of one core: span is wall time
            r->stack = now[i].usStackHighWaterMark;
            if (!strncmp(r->name, "IDLE", 4) && r->core >= 0 && r->core < 2) id[r->core] = r->pct;
        }
        idle[0] = id[0];
        idle[1] = id[1];
        sampled = xTaskGetTickCount();
        xSemaphoreGive(lock);
    }
    for (size_t i = 0; i < n; i++) { prev[i].h = now[i].xHandle; prev[i].run = now[i].ulRunTimeCounter; }
    prev_n = n;
    prev_total = total;
}

static void stats_task(void *arg)
{
    for (;;) {
        sample();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

void stats_start(void)
{
    lock = xSemaphoreCreateMutex();
    now = heap_caps_calloc(MAX_TASKS, sizeof *now, MALLOC_CAP_SPIRAM);
    if (!lock || !now) { ESP_LOGE(TAG, "no memory for the sampler"); return; }
    task_in_psram(stats_task, "stats", 4096, 2, 1);        // under everything that matters, off the WiFi core
}

float stats_idle(int core) { return core == 0 || core == 1 ? idle[core] : -1; }

cJSON *stats_tasks(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *id = cJSON_AddArrayToObject(j, "idle");
    cJSON *list = cJSON_AddArrayToObject(j, "tasks");
    if (!lock) return j;
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON_AddNumberToObject(j, "count", uxTaskGetNumberOfTasks());
    cJSON_AddNumberToObject(j, "age_ms", (double)pdTICKS_TO_MS(xTaskGetTickCount() - sampled));
    cJSON_AddItemToArray(id, cJSON_CreateNumber((int)(idle[0] + 0.5f)));
    cJSON_AddItemToArray(id, cJSON_CreateNumber((int)(idle[1] + 0.5f)));
    for (size_t i = 0; i < rows_n; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", rows[i].name);
        cJSON_AddNumberToObject(t, "core", rows[i].core);
        cJSON_AddNumberToObject(t, "prio", rows[i].prio);
        cJSON_AddNumberToObject(t, "pct", (int)(rows[i].pct * 10 + 0.5f) / 10.0);
        cJSON_AddNumberToObject(t, "stack", rows[i].stack);
        cJSON_AddItemToArray(list, t);
    }
    xSemaphoreGive(lock);
    return j;
}
