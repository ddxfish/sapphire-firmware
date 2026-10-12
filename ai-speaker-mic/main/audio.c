// audio.c - see audio.h.
#include "audio.h"

#include <math.h>
#include <string.h>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"

#include "board.h"
#include "settings.h"

static const char *TAG = "audio";
static bool ok;
static bool halt;
static audio_sink_fn sink;
static SemaphoreHandle_t speaker;
static const esp_afe_sr_iface_t *afe;
static esp_afe_sr_data_t *afe_data;
static int feed_chunk;                 // samples per slot per feed

// the self test listens to the raw slots for a while
static volatile bool measuring;
static double slot_sum[BOARD_CHANNELS];
static size_t slot_count;

static void measure(const int16_t *frames, size_t count)
{
    for (size_t i = 0; i < count; i++)
        for (int s = 0; s < BOARD_CHANNELS; s++)
            slot_sum[s] += (double)frames[i * BOARD_CHANNELS + s] * frames[i * BOARD_CHANNELS + s];
    slot_count += count;
}

// A task whose stack lives in PSRAM: fine for one that never writes flash.
void task_in_psram(TaskFunction_t fn, const char *name, size_t stack, int priority, int core)
{
    StackType_t *mem = heap_caps_malloc(stack, MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mem || !tcb || !xTaskCreateStaticPinnedToCore(fn, name, stack / sizeof(StackType_t), NULL, priority, mem, tcb, core))
        ESP_LOGE(TAG, "task %s did not start", name);
}

static void feed_task(void *arg)
{
    board_audio_t *b = board_audio();
    size_t bytes = feed_chunk * BOARD_CHANNELS * sizeof(int16_t);
    int16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    while (!halt) {
        if (esp_codec_dev_read(b->in, buf, bytes) != 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (measuring) measure(buf, feed_chunk);
        afe->feed(afe_data, buf);
    }
    free(buf);
    vTaskDelete(NULL);
}

static void fetch_task(void *arg)
{
    while (!halt) {
        afe_fetch_result_t *res = afe->fetch_with_delay(afe_data, pdMS_TO_TICKS(500));
        if (!res || res->ret_value == ESP_FAIL || !res->data_size) continue;
        if (sink) sink(res->data, res->data_size / sizeof(int16_t), res->vad_state == VAD_SPEECH);
    }
    vTaskDelete(NULL);
}

static bool afe_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    afe_config_t *cfg = afe_config_init(BOARD_AFE_FORMAT, models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) return false;
    cfg->aec_init = true;
    cfg->aec_mode = AEC_MODE_SR_LOW_COST;
    cfg->ns_init = true;
    cfg->vad_init = true;
    cfg->vad_mode = VAD_MODE_1;
    cfg->vad_min_speech_ms = 128;
    cfg->vad_min_noise_ms = 700;       // this much quiet after speech = the question is over
    cfg->wakenet_init = false;         // the wake word is ours (wake.c)
    cfg->agc_init = settings.agc;                  // the AFE only takes this at start: a change restarts the board
    if (settings.agc) cfg->agc_mode = AFE_AGC_MODE_WEBRTC;
    cfg->afe_ringbuf_size = 20;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe = esp_afe_handle_from_config(cfg);
    afe_data = afe ? afe->create_from_config(cfg) : NULL;
    bool vad_model = cfg->vad_model_name != NULL;
    afe_config_free(cfg);
    if (!afe_data) return false;
    feed_chunk = afe->get_feed_chunksize(afe_data);
    if (afe->get_feed_channel_num(afe_data) != BOARD_CHANNELS) {
        ESP_LOGE(TAG, "the AFE wants %d channels, the board gives %d", afe->get_feed_channel_num(afe_data), BOARD_CHANNELS);
        return false;
    }
    ESP_LOGI(TAG, "AFE %s: %d samples a feed, vad %s", BOARD_AFE_FORMAT, feed_chunk, vad_model ? "model" : "webrtc");
    return true;
}

void audio_start(void)
{
    speaker = xSemaphoreCreateMutex();
    if (!board_audio()->in) { ESP_LOGE(TAG, "no sound: the board did not come up"); return; }   // board_init ran in main, early
    if (!afe_init()) { ESP_LOGE(TAG, "no sound: the AFE did not start"); return; }
    ok = true;
    esp_codec_dev_set_out_vol(board_audio()->out, settings.volume);
    task_in_psram(feed_task, "afe-feed", 8192, 8, 0);
    task_in_psram(fetch_task, "afe-fetch", 12288, 7, 1);     // the wake word network runs on it
}

void audio_stop(void)
{
    halt = true;
    board_amplifier(false);
}

bool audio_ok(void) { return ok; }

void audio_set_mic_gain(float db)
{
    int x2 = (int)(db * 2.0f + 0.5f);
    x2 = x2 < 0 ? 0 : x2 > MIC_GAIN_X2_MAX ? MIC_GAIN_X2_MAX : x2;
    settings.mic_gain_x2 = x2;
    settings_save();
    if (ok) esp_codec_dev_set_in_channel_gain(board_audio()->in, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1), x2 / 2.0f);
}

float audio_mic_gain(void)
{
    return settings.mic_gain_x2 / 2.0f;
}

void audio_set_volume(int level)
{
    level = level < 0 ? 0 : level > 100 ? 100 : level;
    settings.volume = level;
    settings_save_soon();                  // the glass's slider and the volume keys call this from PSRAM stacks
    if (ok) esp_codec_dev_set_out_vol(board_audio()->out, level);
}

int audio_volume(void) { return settings.volume; }

static bool muted;
void audio_set_mute(bool on)
{
    muted = on;
    board_amplifier(!on);
    if (settings.mute != on) { settings.mute = on; settings_save_soon(); }   // kept across a restart
}
bool audio_muted(void) { return muted; }
void audio_set_sink(audio_sink_fn fn) { sink = fn; }

// --- the speaker -------------------------------------------------------------------

bool audio_take_speaker(TickType_t wait) { return ok && xSemaphoreTake(speaker, wait) == pdTRUE; }
void audio_give_speaker(void) { xSemaphoreGive(speaker); }

void audio_play(const int16_t *pcm, size_t samples, int channels)
{
    if (!ok) return;
    if (channels == 2) {                           // the codec is opened mono: mix down in place
        int16_t mono[512];
        for (size_t at = 0; at < samples; at += 1024) {
            size_t frames = (samples - at) / 2 < 512 ? (samples - at) / 2 : 512;
            for (size_t i = 0; i < frames; i++)
                mono[i] = (pcm[at + 2 * i] + pcm[at + 2 * i + 1]) / 2;
            esp_codec_dev_write(board_audio()->out, mono, frames * sizeof(int16_t));
        }
        return;
    }
    esp_codec_dev_write(board_audio()->out, (void *)pcm, samples * sizeof(int16_t));
}

void audio_play_end(void)
{
    static const int16_t quiet[AUDIO_RATE / 20];   // 50 ms of silence flushes the DMA cleanly
    audio_play(quiet, sizeof quiet / sizeof quiet[0], 1);
}

void audio_tone(int hz, int ms)
{
    if (!audio_take_speaker(pdMS_TO_TICKS(3000))) return;
    int16_t buf[320];
    size_t total = (size_t)AUDIO_RATE * ms / 1000;
    for (size_t at = 0; at < total; at += 320) {
        size_t n = total - at < 320 ? total - at : 320;
        for (size_t i = 0; i < n; i++) {
            float edge = fminf(1.0f, fminf((at + i) / 400.0f, (total - at - i) / 400.0f));   // no click
            buf[i] = (int16_t)(8000 * edge * sinf(2 * M_PI * hz * (at + i) / AUDIO_RATE));
        }
        audio_play(buf, n, 1);
    }
    audio_play_end();
    audio_give_speaker();
}

// --- the self test -------------------------------------------------------------------

static void listen_raw(int ms, float out[BOARD_SLOTS])
{
    memset(slot_sum, 0, sizeof slot_sum);
    slot_count = 0;
    measuring = true;
    vTaskDelay(pdMS_TO_TICKS(ms));
    measuring = false;
    for (int s = 0; s < BOARD_CHANNELS; s++)
        out[s] = slot_count ? sqrt(slot_sum[s] / slot_count) : 0;
}

static void tone_task(void *arg)
{
    audio_tone(1000, 900);
    vTaskDelete(NULL);
}

void audio_selftest(audio_selftest_t *t)
{
    memset(t, 0, sizeof *t);
    t->codec_ok = ok;
    if (!ok) return;
    listen_raw(600, t->quiet_slots);
    t->quiet_rms = t->quiet_slots[0];
    // the tone plays on its own task so the measuring window sits inside it
    xTaskCreate(tone_task, "tone", 4096, NULL, 5, NULL);
    vTaskDelay(pdMS_TO_TICKS(150));
    listen_raw(500, t->slot_rms);
    t->tone_rms = t->slot_rms[0];
    vTaskDelay(pdMS_TO_TICKS(400));
}
