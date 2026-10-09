// lv_mem_psram.c - LVGL's heap, on PSRAM. LVGL asks for many small pieces
// (objects, styles, strings); through the IDF heap anything under 4 KB lands
// in internal RAM (SPIRAM_MALLOC_ALWAYSINTERNAL), the scarce one on this board
// (29 KB free, 2026-10-07). So LVGL gets its own allocator: PSRAM first,
// internal only when PSRAM is gone. CONFIG_LV_USE_CUSTOM_MALLOC names this.
#include "lvgl.h"
#include "esp_heap_caps.h"

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { LV_UNUSED(mem); LV_UNUSED(bytes); return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM);
    return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_DEFAULT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    lv_memzero(mon, sizeof *mon);
    mon->total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    mon->free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    mon->used_pct = mon->total_size ? 100 - mon->free_size * 100 / mon->total_size : 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
