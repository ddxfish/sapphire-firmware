// stats.h - where the time goes. FreeRTOS run-time stats, sampled every two
// seconds on a low task: each task's share of a core since the last sample,
// which core it is pinned to, and the idle task's share per core. For the
// console's `tasks` and for `cpu` in /health. Needs
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS and _USE_TRACE_FACILITY.
#pragma once
#include "cJSON.h"

void stats_start(void);
float stats_idle(int core);            // 0..100, the idle share of core 0 or 1 over the last sample; -1 before the first
cJSON *stats_tasks(void);              // {"idle": [i0, i1], "tasks": [{"name", "core", "prio", "pct", "stack"}]}
