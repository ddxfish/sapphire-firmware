// logring.c - see logring.h.
#include "logring.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define MAGIC 0x6c6f6752
// guard: the heap writes its free-list pointers into the first 8 bytes of this block at every boot
// (TLSF's next_free/prev_free of the pool's first free block, before the first malloc splits it):
// the magic there read as a pointer, never as itself (0x3c28db48 twice, 2026-11-05).
typedef struct { uint32_t guard[4], magic, head; char lines[LOGRING_LINES][LOGRING_WIDTH]; } ring_t;

static ring_t *ring;                            // this boot's
static ring_t *last;                            // the boot before, when it was found where this one's is
static vprintf_like_t forward;                  // the console's own vprintf
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static int hook(const char *fmt, va_list ap)
{
    if (ring) {
        char line[LOGRING_WIDTH];
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(line, sizeof line, fmt, ap2);
        va_end(ap2);
        if (n > 0) {
            portENTER_CRITICAL(&mux);
            memcpy(ring->lines[ring->head % LOGRING_LINES], line, LOGRING_WIDTH);
            ring->head++;
            portEXIT_CRITICAL(&mux);
        }
    }
    return forward ? forward(fmt, ap) : 0;
}

void logring_start(void)
{
    ring = heap_caps_malloc(sizeof *ring, MALLOC_CAP_SPIRAM);   // malloc, never calloc: what the last boot left here is read first
    if (!ring) return;
    uint32_t found = ring->magic, head = ring->head;
    if (ring->magic == MAGIC && ring->head < 1000000) {         // the boot before ended with this ring in hand: kept, then this boot starts its own
        last = heap_caps_malloc(sizeof *last, MALLOC_CAP_SPIRAM);
        if (last) memcpy(last, ring, sizeof *last);
    }
    memset(ring, 0, sizeof *ring);
    ring->magic = MAGIC;
    forward = esp_log_set_vprintf(hook);
    ESP_LOGI("logring", "ring at %p: found %08lx, head %lu%s", ring, (unsigned long)found, (unsigned long)head, last ? ": the boot before is kept (GET /log?last=1)" : "");   // the same address each boot of one program is what makes the last boot's lines findable
}

static size_t copy(const ring_t *r, char *out, size_t cap)
{
    size_t at = 0;
    if (!r || cap < 2) { if (cap) out[0] = 0; return 0; }
    unsigned h = r->head, start = h > LOGRING_LINES ? h - LOGRING_LINES : 0;
    for (unsigned i = start; i < h && at + LOGRING_WIDTH + 1 < cap; i++) {
        const char *l = r->lines[i % LOGRING_LINES];
        size_t n = strnlen(l, LOGRING_WIDTH - 1);
        while (n && (l[n - 1] == '\n' || l[n - 1] == '\r')) n--;
        memcpy(out + at, l, n);
        at += n;
        out[at++] = '\n';
    }
    out[at] = 0;
    return at;
}

size_t logring_copy(char *out, size_t cap) { return copy(ring, out, cap); }
size_t logring_copy_last(char *out, size_t cap) { return copy(last, out, cap); }
bool logring_has_last(void) { return last != NULL; }
