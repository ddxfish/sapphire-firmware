// wake.c - see wake.h. Everything here runs on the audio pipeline's task
// except the send, which has its own.
#include "wake.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio.h"
#include "light.h"
#include "link.h"
#include "mww.h"
#include "ui.h"

static const char *TAG = "wake";

#define MODEL_NAME "hey_jarvis"        // the open test model, until "hey sapphire" is trained
#define MAX_RECORD_S 60                // the buffer: a room recording through /audio/listen (1.9 MB of PSRAM)
#define QUESTION_MAX_S 15              // a question after the wake word
#define PREROLL_MS 250                 // the moment before the wake word ended, kept
#define NO_SPEECH_MS 4000              // woke, and nobody said anything
#define QUIET_END_MS 700               // silence after speech that ends the question
#define PREROLL (AUDIO_RATE * PREROLL_MS / 1000)
#define MAX_SAMPLES ((size_t)AUDIO_RATE * MAX_RECORD_S)

extern const uint8_t model_start[] asm("_binary_hey_jarvis_tflite_start");
extern const uint8_t model_end[] asm("_binary_hey_jarvis_tflite_end");

// A model sent through PUT /wakeword/model lives on the "wake" partition (SPIFFS at /wake; "model" is ESP-SR's,
// never touched): wake.tflite beside wake.json (name, threshold, sliding_window, step_ms, phrase). It is read into
// PSRAM at boot and wins over the embedded one; without it, or if it will not load, the embedded model listens.
#define MODEL_DIR "/wake"
#define STORED_MODEL MODEL_DIR "/wake.tflite"
#define STORED_META MODEL_DIR "/wake.json"
#define STORED_ARENA 65536            // a trained mixednet may want more than the open model's 22860
static char stored_name[48];
static char stored_phrase[64];
static float stored_threshold = 0.97f;
static uint8_t *stored;               // PSRAM
static size_t stored_size;
static bool store_ok;

static bool store_mount(void)
{
    if (store_ok) return true;
    esp_vfs_spiffs_conf_t conf = {.base_path = MODEL_DIR, .partition_label = "wake", .max_files = 4, .format_if_mount_failed = true};
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) { ESP_LOGW(TAG, "no model store: %s", esp_err_to_name(err)); return false; }
    store_ok = true;
    return true;
}

static bool read_file(const char *path, uint8_t **out, size_t *size)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *data = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM);
    if (!data) { fclose(f); return false; }
    size_t got = fread(data, 1, st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) { free(data); return false; }
    data[got] = 0;
    *out = data;
    *size = got;
    return true;
}

static bool load_stored(mww_config_t *cfg)
{
    if (!store_mount()) return false;
    uint8_t *meta = NULL;
    size_t meta_size = 0;
    if (!read_file(STORED_MODEL, &stored, &stored_size)) return false;
    cJSON *j = read_file(STORED_META, &meta, &meta_size) ? cJSON_Parse((const char *)meta) : NULL;
    free(meta);
    const cJSON *v;
    strlcpy(stored_name, (j && cJSON_IsString(v = cJSON_GetObjectItem(j, "name"))) ? v->valuestring : "stored", sizeof stored_name);
    strlcpy(stored_phrase, (j && cJSON_IsString(v = cJSON_GetObjectItem(j, "phrase"))) ? v->valuestring : "", sizeof stored_phrase);
    stored_threshold = (j && cJSON_IsNumber(v = cJSON_GetObjectItem(j, "threshold"))) ? (float)v->valuedouble : 0.97f;
    int window = (j && cJSON_IsNumber(v = cJSON_GetObjectItem(j, "sliding_window"))) ? v->valueint : 5;
    int step = (j && cJSON_IsNumber(v = cJSON_GetObjectItem(j, "step_ms"))) ? v->valueint : 10;
    cJSON_Delete(j);
    cfg->model = stored; cfg->model_size = stored_size; cfg->probability_cutoff = stored_threshold;
    cfg->sliding_window_size = window < 1 ? 5 : window; cfg->feature_step_ms = step < 1 ? 10 : step;
    cfg->tensor_arena_size = STORED_ARENA;
    return true;
}

typedef enum { IDLE, RECORDING, SENDING } state_t;

static bool enabled = true;
static volatile bool forget;            // the model is to be reset, by the task that feeds it
static bool running;
static state_t state;
static bool for_listen;                // the recording is for /audio/listen, not the wake word
static bool plain;                     // /audio/listen?vad=false: the room for max_s seconds, no speech rules
static int16_t *buf;                   // PSRAM, MAX_SAMPLES
static size_t have;
static size_t limit;
static int16_t ring[PREROLL];
static size_t ring_at;
static bool speech_seen;
static int64_t began_us, last_speech_us;
static SemaphoreHandle_t done;         // given when a recording ends
static SemaphoreHandle_t send_now;
static SemaphoreHandle_t claim_now;    // given at the wake word: ask Sapphire for the room
static bool ended_by_speech;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void begin(bool listen, size_t max_samples)
{
    // the moment before the wake word ended is kept, so a fast talker is not cut
    have = 0;
    for (size_t i = 0; i < PREROLL; i++) buf[have++] = ring[(ring_at + i) % PREROLL];
    limit = max_samples > MAX_SAMPLES ? MAX_SAMPLES : max_samples;
    speech_seen = false;
    began_us = last_speech_us = now_ms();
    for_listen = listen;
    plain = false;
    state = RECORDING;
    light_cue("listening");
    if (!listen) xSemaphoreGive(claim_now);          // alongside the recording, never before it
}

void wake_standdown(const char *who)
{
    if (state != RECORDING || for_listen) return;
    ESP_LOGI(TAG, "standing down: the room is %s's", who && who[0] ? who : "another's");
    state = IDLE;                                     // on_audio stops filling the buffer
    mww_reset();
    light_cue("idle");
}

static void end(bool spoke)
{
    ended_by_speech = spoke;
    if (for_listen) {
        state = SENDING;               // held until wake_record_done
        xSemaphoreGive(done);
        return;
    }
    if (!spoke) {
        ESP_LOGI(TAG, "woke, heard nothing");
        state = IDLE;
        light_cue("idle");
        return;
    }
    state = SENDING;
    light_cue("thinking");
    xSemaphoreGive(send_now);
}

static void on_audio(const int16_t *pcm, size_t samples, bool speech)
{
    for (size_t i = 0; i < samples; i++) {         // always: the pre-roll ring
        ring[ring_at] = pcm[i];
        ring_at = (ring_at + 1) % PREROLL;
    }
    if (state == IDLE) {
        if (forget) { forget = false; mww_reset(); }
        if (enabled && mww_feed(pcm, samples)) {
            ESP_LOGI(TAG, "wake word");
            begin(false, (size_t)AUDIO_RATE * QUESTION_MAX_S);
        }
        return;
    }
    if (state != RECORDING) return;
    size_t n = limit - have < samples ? limit - have : samples;
    memcpy(buf + have, pcm, n * sizeof(int16_t));
    have += n;
    int64_t t = now_ms();
    if (plain) {                                      // the room: runs to the limit, whatever is said
        if (have >= limit) end(true);
        return;
    }
    if (speech) { speech_seen = true; last_speech_us = t; }
    if (have >= limit) end(speech_seen);
    else if (speech_seen && t - last_speech_us > QUIET_END_MS) end(true);
    else if (!speech_seen && t - began_us > NO_SPEECH_MS) end(false);
}

static void send_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(send_now, portMAX_DELAY);
        char heard[160];
        ESP_LOGI(TAG, "sending %.1f s", have / (float)AUDIO_RATE);
        bool ok = link_send_voice(buf, have, heard, sizeof heard);
        if (ok) ESP_LOGI(TAG, "she heard: %s", heard[0] ? heard : "(nothing)");
        if (ok && heard[0]) ui_heard(heard);
        else light_cue("error");
        if (ok && !heard[0]) light_cue("idle");   // silence: no turn, so no cue will come
        mww_reset();
        state = IDLE;
    }
}

static void claim_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(claim_now, portMAX_DELAY);
        char who[64];
        if (!link_claim_wake(who, sizeof who)) wake_standdown(who);
    }
}

void wake_start(void)
{
    done = xSemaphoreCreateBinary();
    send_now = xSemaphoreCreateBinary();
    claim_now = xSemaphoreCreateBinary();
    buf = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "no memory for recordings"); return; }
    mww_config_t cfg = {
        .model = model_start, .model_size = model_end - model_start,
        .probability_cutoff = 0.97f, .sliding_window_size = 5, .tensor_arena_size = 22860,
        .feature_step_ms = 10,
    };
    bool have_stored = load_stored(&cfg);
    if (have_stored && !mww_init(&cfg)) {
        ESP_LOGE(TAG, "the stored model '%s' did not load; back to the embedded one", stored_name);
        stored_name[0] = 0;
        cfg.model = model_start; cfg.model_size = model_end - model_start; cfg.probability_cutoff = 0.97f;
        cfg.sliding_window_size = 5; cfg.tensor_arena_size = 22860; cfg.feature_step_ms = 10;
        have_stored = false;
    }
    if (!have_stored && !mww_init(&cfg)) { ESP_LOGE(TAG, "the wake word model did not load"); return; }
    running = audio_ok();
    audio_set_sink(on_audio);
    task_in_psram(send_task, "wake-send", 8192, 5, 0);     // stacks in PSRAM, TCBs internal; no flash writes on them
    task_in_psram(claim_task, "wake-claim", 8192, 5, 0);
    ESP_LOGI(TAG, "listening for '%s' (%s)", wake_model_name(), stored_name[0] ? "stored" : "embedded");
}

bool wake_enabled(void) { return enabled; }
void wake_trigger(void)
{
    if (!audio_ok() || state != IDLE) return;
    ESP_LOGI(TAG, "listening by hand");
    begin(false, (size_t)AUDIO_RATE * QUESTION_MAX_S);
}
void wake_enable(bool on) { enabled = on; if (!on) forget = true; }   // called from the glass's task and the server's: never the model itself
bool wake_running(void) { return running && audio_ok(); }
bool wake_busy(void) { return state != IDLE; }
const char *wake_model_name(void) { return stored_name[0] ? stored_name : MODEL_NAME; }
float wake_model_threshold(void) { return stored_name[0] ? stored_threshold : 0.97f; }
bool wake_model_stored(void) { return stored_name[0] != 0; }

// A new model from PUT /wakeword/model: written beside its manifest, then swapped in by name. The caller restarts
// the board so the audio pipeline starts clean on it (a few seconds).
bool wake_model_store(const uint8_t *data, size_t size, const char *name, const char *phrase, float threshold,
                      int sliding_window, int step_ms, char *why, size_t why_len)
{
    if (!store_mount()) { strlcpy(why, "this board has no model partition", why_len); return false; }
    if (size < 4096 || size > 1536 * 1024 || memcmp(data + 4, "TFL3", 4) != 0) {
        strlcpy(why, "not a .tflite model (4 KB to 1.5 MB, TFL3 header)", why_len);
        return false;
    }
    FILE *f = fopen(MODEL_DIR "/new.tflite", "wb");
    if (!f || fwrite(data, 1, size, f) != size) {
        if (f) fclose(f);
        remove(MODEL_DIR "/new.tflite");
        strlcpy(why, "could not write the model (partition full?)", why_len);
        return false;
    }
    fclose(f);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", name);
    cJSON_AddStringToObject(j, "phrase", phrase ? phrase : "");
    cJSON_AddNumberToObject(j, "threshold", threshold);
    cJSON_AddNumberToObject(j, "sliding_window", sliding_window);
    cJSON_AddNumberToObject(j, "step_ms", step_ms);
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    f = fopen(MODEL_DIR "/new.json", "wb");
    bool ok = f && text && fputs(text, f) >= 0;
    if (f) fclose(f);
    cJSON_free(text);
    if (!ok) { remove(MODEL_DIR "/new.tflite"); strlcpy(why, "could not write the manifest", why_len); return false; }
    remove(STORED_MODEL); remove(STORED_META);
    if (rename(MODEL_DIR "/new.tflite", STORED_MODEL) != 0 || rename(MODEL_DIR "/new.json", STORED_META) != 0) {
        strlcpy(why, "could not place the model", why_len);
        return false;
    }
    return true;
}

bool wake_model_forget(void)
{
    if (!store_mount()) return false;
    remove(STORED_MODEL); remove(STORED_META);
    return true;
}

bool wake_record_now(int max_s, bool use_vad, recording_t *out)
{
    if (!audio_ok() || state != IDLE) return false;
    xSemaphoreTake(done, 0);
    begin(true, (size_t)AUDIO_RATE * max_s);
    plain = !use_vad;
    xSemaphoreTake(done, portMAX_DELAY);
    out->pcm = buf;
    out->samples = ended_by_speech ? have : 0;
    return true;
}

void wake_record_done(recording_t *rec)
{
    mww_reset();
    state = IDLE;
    light_cue("idle");
}
