// capture.c - see capture.h. Two halves in PSRAM: the audio task fills one while the sender posts the other.
// A piece the sender has not finished with by the time the next is full is dropped and counted, never
// waited for: the audio task is the AFE's and must not block.
#include "capture.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "audio.h"
#include "link.h"
#include "wake.h"

static const char *TAG = "capture";

#define CHUNK_LEAST_S 5
#define CHUNK_MOST_S 60
#define SECONDS_MOST (24 * 3600)
#define FIRES_MOST 40                  // stamped per piece (one header line); past that they are only counted
#define LEAST_SAMPLES (AUDIO_RATE / 2) // a last piece shorter than this is not worth a request
#define END (-1)                       // the sender's cue to let the buffers go

typedef struct { uint32_t at; uint16_t score; } fire_t;    // sample offset within the piece, probability x 1000
typedef struct {
    int16_t *pcm;                      // PSRAM, chunk_samples
    size_t have;
    fire_t fires[FIRES_MOST];
    int n_fires, fires_total;
    int no;                            // the piece's number within this capture
    volatile bool busy;                // the sender is on it
} half_t;

static half_t halves[2];
static volatile int filling;           // the half the audio task writes into
static size_t chunk_samples;
static volatile bool active, test_mode, stopping;
static int64_t began_ms, end_ms;
static int chunk_s;
static int sent, dropped, lost, fires_all, next_no;
static char id[24];
static QueueHandle_t ready;            // half index or END, depth 1

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// The filling half is full, or the capture ends with something in it: to the sender, unless the sender is
// still on the other half or has not taken the last one (then this piece is lost and the half is reused).
static void hand_over(void)
{
    int i = filling, other = 1 - filling;
    halves[i].no = next_no++;
    if (!halves[other].busy && xQueueSend(ready, &i, 0) == pdTRUE) {
        filling = other;
    } else {
        dropped++;
        ESP_LOGW(TAG, "piece %d dropped: the one before is still on its way", halves[i].no);
    }
    halves[filling].have = 0;
    halves[filling].n_fires = 0;
    halves[filling].fires_total = 0;
}

static void finish(void)
{
    active = false;
    if (halves[filling].have >= LEAST_SAMPLES) hand_over();
    stopping = true;
    int end = END;
    xQueueSend(ready, &end, 0);        // after the last piece when that went in; if the queue is full, the sender sees `stopping` itself
}

void capture_feed(const int16_t *pcm, size_t samples)
{
    if (!active) return;
    half_t *h = &halves[filling];
    while (samples > 0 && active) {
        size_t n = chunk_samples - h->have;
        if (n > samples) n = samples;
        memcpy(h->pcm + h->have, pcm, n * sizeof(int16_t));
        h->have += n;
        pcm += n;
        samples -= n;
        if (h->have >= chunk_samples) { hand_over(); h = &halves[filling]; }
    }
    if (now_ms() >= end_ms) { ESP_LOGI(TAG, "time is up"); finish(); }
}

void capture_fire(float score)
{
    if (!active) return;
    half_t *h = &halves[filling];
    h->fires_total++;
    fires_all++;
    if (h->n_fires < FIRES_MOST) {
        h->fires[h->n_fires].at = (uint32_t)h->have;
        h->fires[h->n_fires].score = (uint16_t)(score * 1000.0f + 0.5f);
        h->n_fires++;
    }
}

static void let_go(void)
{
    for (int i = 0; i < 2; i++) { free(halves[i].pcm); halves[i].pcm = NULL; halves[i].have = 0; }
    stopping = false;
    ESP_LOGI(TAG, "over: %d pieces sent, %d dropped, %d lost, %d fires", sent, dropped, lost, fires_all);
}

static void sender_task(void *arg)
{
    for (;;) {
        int i;
        if (xQueueReceive(ready, &i, pdMS_TO_TICKS(250)) == pdTRUE && i >= 0 && halves[i].pcm) {
            half_t *h = &halves[i];
            h->busy = true;
            char fires[FIRES_MOST * 16 + 8];
            size_t at = 0;
            fires[at++] = '[';
            for (int k = 0; k < h->n_fires && at < sizeof fires - 20; k++)
                at += snprintf(fires + at, sizeof fires - at, "%s[%lu,%u]", k ? "," : "", (unsigned long)h->fires[k].at, h->fires[k].score);
            fires[at++] = ']';
            fires[at] = 0;
            int status = link_send_room(h->pcm, h->have, id, h->no, fires, h->fires_total, test_mode, wake_model_name());
            if (status == 200) sent++;
            else if (status == 204) {          // taken, and nobody on her side is collecting: the capture ends
                ESP_LOGW(TAG, "nobody is collecting the room on Sapphire's side: capture ends");
                active = false; stopping = true;
            } else if (status >= 400 && status < 500) {
                ESP_LOGW(TAG, "Sapphire refused the room (%d): capture ends", status);
                active = false; stopping = true;
            } else {
                lost++;
            }
            h->busy = false;
        }
        if (stopping && !active && uxQueueMessagesWaiting(ready) == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));    // a feed call that was mid-piece is over by now
            let_go();
        }
    }
}

void capture_init(void)
{
    ready = xQueueCreate(1, sizeof(int));
    task_in_psram(sender_task, "capture", 8192, 4, 1);   // core 1, under the link's tasks; no flash writes on it
}

bool capture_start(int seconds, int chunk, bool test, char *why, size_t why_len)
{
    if (active || stopping) { strlcpy(why, "a capture is already going on", why_len); return false; }
    if (!audio_ok()) { strlcpy(why, "no sound is coming in from the microphones", why_len); return false; }
    if (seconds < 1) seconds = 1;
    if (seconds > SECONDS_MOST) seconds = SECONDS_MOST;
    if (chunk < CHUNK_LEAST_S) chunk = CHUNK_LEAST_S;
    if (chunk > CHUNK_MOST_S) chunk = CHUNK_MOST_S;
    chunk_s = chunk;
    chunk_samples = (size_t)AUDIO_RATE * chunk;
    for (int i = 0; i < 2; i++) {
        halves[i].pcm = heap_caps_malloc(chunk_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        halves[i].have = 0; halves[i].n_fires = 0; halves[i].fires_total = 0; halves[i].busy = false;
    }
    if (!halves[0].pcm || !halves[1].pcm) {
        for (int i = 0; i < 2; i++) { free(halves[i].pcm); halves[i].pcm = NULL; }
        strlcpy(why, "not enough memory for two pieces of that length", why_len);
        return false;
    }
    sent = dropped = lost = fires_all = next_no = 0;
    filling = 0;
    test_mode = test;
    began_ms = now_ms();
    end_ms = began_ms + (int64_t)seconds * 1000;
    snprintf(id, sizeof id, "%lld", (long long)began_ms);
    xQueueReset(ready);
    active = true;
    ESP_LOGI(TAG, "the room for %d s in pieces of %d s%s", seconds, chunk, test ? " (test: fires only counted)" : "");
    return true;
}

void capture_stop(void)
{
    if (!active) return;
    ESP_LOGI(TAG, "stopped");
    finish();
}

bool capture_active(void) { return active; }
bool capture_test(void) { return active && test_mode; }

cJSON *capture_json(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "active", active);
    cJSON_AddBoolToObject(j, "test", active && test_mode);
    if (active || stopping) {
        cJSON_AddStringToObject(j, "id", id);
        cJSON_AddNumberToObject(j, "seconds", (double)((now_ms() - began_ms) / 1000));
        cJSON_AddNumberToObject(j, "left_s", (double)(end_ms > now_ms() ? (end_ms - now_ms()) / 1000 : 0));
        cJSON_AddNumberToObject(j, "chunk_s", chunk_s);
    }
    cJSON_AddNumberToObject(j, "sent", sent);
    cJSON_AddNumberToObject(j, "dropped", dropped);
    cJSON_AddNumberToObject(j, "lost", lost);
    cJSON_AddNumberToObject(j, "fires", fires_all);
    return j;
}
