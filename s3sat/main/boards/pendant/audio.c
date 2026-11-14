// audio.c - see audio.h.
#include "audio.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_common.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"

#include "board.h"
#include "power.h"
#include "settings.h"

static const char *TAG = "audio";
static bool ok;
static volatile bool halt, suspended;
static audio_sink_fn sink;
static SemaphoreHandle_t speaker;
static const esp_afe_sr_iface_t *afe;
static esp_afe_sr_data_t *afe_data;
static int feed_chunk;                 // samples per channel per feed
static int32_t gain_q15 = 32768;       // the speaker's volume as a multiplier
static int32_t mic_q15 = 32768;        // the mic's digital gain
static bool muted;
static int spk_rate = SPEAKER_RATE;
int audio_speaker_rate(void) { return spk_rate; }
static volatile float mic_rms;           // the raw (gained) mic over the last chunk
float audio_mic_rms(void) { return mic_rms; }
static uint32_t dropped_seen;            // board_mic_dropped() already folded into ref_r
static volatile int feed_max_ms, feed_slow, pops_stale;
void audio_mic_health(int *dropped_ms, int *feed_max, int *slow, int *stale)
{
    *dropped_ms = (int)(board_mic_dropped() / (AUDIO_RATE / 1000));
    *feed_max = feed_max_ms; *slow = feed_slow; *stale = pops_stale;
}

// --- the echo reference: a ring the player writes and the feed task reads `delay` later ---
// Both run at 16 kHz once sound flows, so the gap between the write and the read positions
// holds; while nothing plays only the read side moves and reads zeros (every read zeroes
// its slot), and the next sentence is anchored afresh at read + delay.
#define REF_RING 32768                 // 2 s, a power of two
static int16_t *ref;
static volatile uint32_t ref_r, ref_w; // sample counters; ring index = counter & (REF_RING - 1)
static volatile int delay_samples;
static volatile bool aec_on = true;
static volatile uint32_t burst_at;     // aec_measure: the read counter when the burst's first sample was written
static volatile bool burst_mark;

// Re-anchored only after a real pause: while a sentence plays the write side wanders a chunk
// either side of the read side (three speaker samples make two reference samples, and the
// pops come in 512s), and anchoring on every small dip skipped the reference ahead mid-sentence
// and lost the echo for the rest of it (the 24 kHz change, 2026-11-02).
// Anchored on the amplifier's own clock: `written` counts the speaker samples handed to the
// DMA since the stream last started, and the DMA plays them at SPEAKER_RATE from `t0`. A write
// that arrives when everything written has already played means the DMA drained and silence
// came out of the air meanwhile (auto_clear), so the sound starts afresh and so does the
// reference. A write that arrives while sound is still queued continues the stream: no anchor.
// (A fixed "40 ms since the last write" rule re-anchored inside the 50 ms of silence appended
// to every sentence and misaligned the next one; two runs in three lost the echo, 2026-11-02.)
// The read side is the MIC'S CLOCK, not the pop count: the mic's DMA captures at AUDIO_RATE
// without pause, so the index of the sample being captured now is wall time since mic_t0. The
// feed task pops 512 at a time and can run late (the first sentence lands over TLS on its own
// core at that very moment): an anchor taken from the pops was off by the backlog, two or three
// chunks, more than the canceller spans, and one run in three lost the echo (2026-11-02).
// mic_t0 is resynced at every pop the feed task had to WAIT for (then the chunk is fresh and
// the newest sample's index is ref_r), never at one it found waiting (a backlog).
static volatile int64_t mic_t0_us;     // when sample 0 of the pop count was captured
#define MIC_HOLD_MAX_MS 100            // a negative aec_delay_ms holds the mic up to this long
#define MIC_HOLD_RING 2048             // 128 ms, a power of two
#define MIC_WINDOW 6                   // estimates kept: one descriptor-phase cycle (five pops) and one over
static int64_t mic_t0_seen[MIC_WINDOW];
static int mic_t0_at;
static int64_t mic_first_us;           // the first exact estimate: with ref_r, the mic's real rate (/health mic_hz)
static uint32_t mic_first_r;
static uint64_t spk_first;             // the speaker's clock, measured like the mic's
static int64_t spk_first_us;
static int ref_ppm;
int audio_ref_ppm(void) { return ref_ppm; }
// OFF: the nominal 2:3 keeps the interpolator's phase pattern fixed, one filter the canceller
// absorbs. With the measured ratio the phase walks and a two-point interpolator's high band
// wobbles up to 6 dB with it: the echo path kept changing under the canceller, 32x fell to
// 4.6x on the same build (2026-11-02). The clocks measure within ~100 ppm of each other
// (the earlier "800 ppm" was a biased start mark), a few ms over a minute of speech: left
// alone. A drift fix would need a phase-flat (windowed sinc) resampler, not this.
static bool ref_ratio_on = false;       // bench only: /mic?ref_ratio=on
void audio_ref_ratio(bool on) { ref_ratio_on = on; }
bool audio_ref_ratio_on(void) { return ref_ratio_on; }
float audio_spk_hz(void)
{
    uint64_t f; int64_t t;
    board_spk_sent(&f, &t);
    if (!spk_first_us || !t) return 0;
    int64_t span = t - spk_first_us;
    return span < 2000000 ? 0 : (float)((double)(f - spk_first) * 1000000.0 / (double)span);
}

float audio_mic_hz(void)
{
    if (!mic_first_us || !mic_t0_us) return 0;
    int64_t now_capture = mic_t0_us + (int64_t)ref_r * 1000000 / AUDIO_RATE;   // when the newest popped sample was captured, by the clock's window
    int64_t span = now_capture - mic_first_us;
    return span < 2000000 ? 0 : (float)((double)(ref_r - mic_first_r) * 1000000.0 / (double)span);
}
static int64_t stream_t0_us;           // when the DMA started playing this stream
static uint64_t written;               // speaker samples written to it since then
static inline void ref_anchor(size_t about_to_write)
{
    int64_t now = esp_timer_get_time();
    // SIGNED, clamped: the stream's start is up to one descriptor in the FUTURE, and an unsigned
    // "now minus start" went huge for the writes of those first milliseconds, re-anchored on each
    // and overwrote the reference's first chunk with its second: a one- or two-chunk lead for the
    // whole sentence, 11 or 21 ms, 2.5x to 10x on five sentences in six (0.1.31).
    int64_t played = stream_t0_us ? (now - stream_t0_us) * spk_rate / 1000000 : 0;
    if (played < 0) played = 0;
    if (!stream_t0_us || played >= (int64_t)written) {
        // The first sample written now goes into the descriptor after the one playing (the
        // driver's queue is one short of the ring), so it comes out when that one ends: one
        // descriptor after its start, which the amplifier's ISR timestamped (board.c). Anchoring
        // on "now" instead left 0-7 ms of slop per sentence, a different alignment every time
        // and the filter re-converging from it: identical sentences cut 6x to 85x (0.1.30).
        uint64_t frames; int64_t at;
        board_spk_sent(&frames, &at);
        int64_t t_play = (at ? at : now) + (int64_t)BOARD_SPK_DESC_FRAMES * 1000000 / spk_rate;
        uint32_t mic_then = mic_t0_us ? (uint32_t)((t_play - mic_t0_us) * AUDIO_RATE / 1000000) : ref_r;
        ref_w = mic_then + (delay_samples > 0 ? delay_samples : 0);   // a negative delay holds the MIC (feed_task), the reference stays at the mic's clock
        stream_t0_us = t_play;
        written = 0;
    }
    written += about_to_write;
}

// The reference is resampled from the speaker's frames to the mic's samples by the RATIO OF
// THE TWO CLOCKS AS MEASURED, not the nominal 2:3: the PDM divider runs ~800 ppm slow and
// the amplifier's does its own thing, and at 2:3 the reference walked ahead of the echo by
// 8 ms every 10 s of a long reply (0.1.26). Linear interpolation; `frac` is where the next
// reference sample sits between the previous frame and this one, in 1/65536 of a frame.
static uint32_t ref_step_q16(void)
{
    uint32_t nominal = (uint32_t)(65536ULL * spk_rate / AUDIO_RATE);          // frames per reference sample
    float m = audio_mic_hz(), s = audio_spk_hz();
    if (!ref_ratio_on || m < 1 || s < 1) { ref_ppm = 0; return nominal; }
    double step = (double)s / m * 65536.0, limit = nominal * 0.005;            // frames per mic sample, measured
    if (step > nominal + limit) step = nominal + limit;                       // the rates are never half a percent off: a bad reading
    if (step < nominal - limit) step = nominal - limit;
    ref_ppm = (int)((step / nominal - 1.0) * 1000000.0);
    return (uint32_t)(step + 0.5);
}

static void ref_push(const int16_t *pcm, size_t n)
{
    static int16_t prev;
    static uint32_t frac;
    if (!ref) return;
    ref_anchor(n);
    if (burst_mark) { burst_mark = false; burst_at = ref_r; }
    uint32_t w = ref_w, step = ref_step_q16();
    for (size_t i = 0; i < n; i++) {
        int16_t s = muted ? 0 : pcm[i];
        while (frac < 65536) {
            ref[w++ & (REF_RING - 1)] = (int16_t)(prev + (((int32_t)s - prev) * (int32_t)frac >> 16));
            frac += step;
        }
        frac -= 65536;
        prev = s;
    }
    ref_w = w;
}

static inline int16_t ref_pop(void)
{
    uint32_t r = ref_r++;
    int16_t v = ref[r & (REF_RING - 1)];
    ref[r & (REF_RING - 1)] = 0;
    return v;
}

// --- measuring: the raw mic, for the self test and the delay measurement ---------------
static volatile bool measuring;
static double slot_sum[BOARD_CHANNELS];
static size_t slot_count;
#define RAW_MAX (AUDIO_RATE * 2)
static int16_t *raw;                   // PSRAM, filled while raw_on
static volatile size_t raw_have;
static volatile bool raw_on;
static uint32_t raw_start_r;

static void measure(const int16_t *frames, size_t count)
{
    for (size_t i = 0; i < count; i++)
        for (int s = 0; s < BOARD_CHANNELS; s++)
            slot_sum[s] += (double)frames[i * BOARD_CHANNELS + s] * frames[i * BOARD_CHANNELS + s];
    slot_count += count;
}

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
    size_t bytes = feed_chunk * sizeof(int16_t);
    int16_t *mic = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *frame = heap_caps_malloc(bytes * BOARD_CHANNELS, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *hold = heap_caps_calloc(MIC_HOLD_RING, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);   // the mic held back (a negative delay): the reference leads by that much more
    uint32_t hold_at = 0;
    while (!halt) {
        size_t got = 0;
        int64_t before = esp_timer_get_time();
        if (suspended || i2s_channel_read(b->rx, mic, bytes, &got, 500) != ESP_OK || got != bytes) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        int64_t after = esp_timer_get_time();
        bool fresh = after - before > 4000;              // it waited for the chunk: the newest sample was captured just now
        if (!fresh) pops_stale++;
        uint32_t dropped = board_mic_dropped();          // a hole before this chunk: the index skips it, so the reference stays
        if (dropped != dropped_seen) { ref_r += dropped - dropped_seen; dropped_seen = dropped; }   // aligned with what the mic really heard
        int hold_n = delay_samples < 0 && hold ? -delay_samples : 0;
        for (int i = 0; i < feed_chunk; i++) {
            int32_t v = (mic[i] * mic_q15) >> 15;
            int16_t m = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
            if (hold_n) { int16_t h = hold[(hold_at - hold_n) & (MIC_HOLD_RING - 1)]; hold[hold_at++ & (MIC_HOLD_RING - 1)] = m; m = h; }
            frame[2 * i] = m;
            int16_t r = ref_pop();
            frame[2 * i + 1] = aec_on ? r : 0;
        }
        if (raw_on) {
            size_t have = raw_have, n = RAW_MAX - have < (size_t)feed_chunk ? RAW_MAX - have : feed_chunk;
            for (size_t i = 0; i < n; i++) raw[have + i] = frame[2 * i];
            raw_have = have + n;
            if (raw_have >= RAW_MAX) raw_on = false;
        }
        if (!spk_first_us && esp_timer_get_time() > 10000000) { int64_t t; board_spk_sent(&spk_first, &t); spk_first_us = t; }   // the speaker's clock, marked ten seconds in
        if (fresh) {
            // ref_r = the pops so far = the index of the sample being captured now. The read returns
            // at a DMA descriptor's end, 0-16 ms after the chunk's last sample (320-frame descriptors
            // against 512-sample pops: the phase repeats every five, one of them exact), and later
            // still when the core was busy. Every error makes the clock look LATE, and a late clock
            // puts the reference AHEAD of the echo, which the canceller spans; an EARLY clock would put
            // it behind, which it cannot model. So: the earliest of the last six estimates, never an
            // older one (an all-time minimum went stale as the mic's clock drifted and the reference
            // fell behind for good: four bad sentences in four, 2026-11-02).
            int64_t t0 = after - (int64_t)ref_r * 1000000 / AUDIO_RATE;
            mic_t0_seen[mic_t0_at] = t0;
            mic_t0_at = (mic_t0_at + 1) % MIC_WINDOW;
            int64_t least = t0;
            for (int i = 0; i < MIC_WINDOW; i++) if (mic_t0_seen[i] && mic_t0_seen[i] < least) least = mic_t0_seen[i];
            mic_t0_us = least;
            if (!mic_first_us && after > 10000000) { mic_first_us = least + (int64_t)ref_r * 1000000 / AUDIO_RATE; mic_first_r = ref_r; }   // marked ten seconds in, the window full: the first pops' estimates were late
        }
        { double e = 0; for (int i = 0; i < feed_chunk; i++) e += (double)frame[2 * i] * frame[2 * i]; mic_rms = sqrtf((float)(e / feed_chunk)); }
        if (measuring) measure(frame, feed_chunk);
        int64_t f0 = esp_timer_get_time();
        afe->feed(afe_data, frame);
        int took = (int)((esp_timer_get_time() - f0) / 1000);
        if (took > feed_max_ms) feed_max_ms = took;
        if (took > 32) feed_slow++;                        // longer than the chunk it ate: the ring is filling behind it
    }
    free(mic);
    free(frame);
    free(hold);
    vTaskDelete(NULL);
}

static volatile float out_rms;
static volatile bool out_measuring;
static double out_sum;
static size_t out_count;
static volatile int64_t speech_log[64];     // when the VAD last said speech, a ring of chunk times
static volatile int speech_at;

static void fetch_task(void *arg)
{
    while (!halt) {
        afe_fetch_result_t *res = afe->fetch_with_delay(afe_data, pdMS_TO_TICKS(500));
        if (!res || res->ret_value == ESP_FAIL || !res->data_size) continue;
        size_t n = res->data_size / sizeof(int16_t);
        double e = 0;
        for (size_t i = 0; i < n; i++) e += (double)res->data[i] * res->data[i];
        out_rms = n ? sqrtf((float)(e / n)) : 0;
        if (out_measuring) { out_sum += e; out_count += n; }
        if (res->vad_state == VAD_SPEECH) { speech_log[speech_at & 63] = esp_timer_get_time(); speech_at++; }
        if (sink) sink(res->data, n, res->vad_state == VAD_SPEECH);
    }
    vTaskDelete(NULL);
}

static bool afe_init(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    afe_config_t *cfg = afe_config_init(BOARD_AFE_FORMAT, models, AFE_TYPE_SR, settings.afe_mode ? AFE_MODE_HIGH_PERF : AFE_MODE_LOW_COST);
    if (!cfg) return false;
    cfg->aec_init = true;
    cfg->aec_mode = settings.afe_mode ? AEC_MODE_SR_HIGH_PERF : AEC_MODE_SR_LOW_COST;   // the mode: settings.afe_mode, POST /mic?afe_mode=, a restart applies it
    cfg->aec_filter_length = settings.aec_filter;      // the slack the canceller spans past the delay; costs CPU as it grows
    cfg->ns_init = true;
    cfg->vad_init = true;
    cfg->vad_mode = VAD_MODE_1;
    cfg->vad_min_speech_ms = 128;
    cfg->vad_min_noise_ms = 700;
    cfg->wakenet_init = false;
    cfg->agc_init = settings.agc;
    if (settings.agc) cfg->agc_mode = AFE_AGC_MODE_WEBRTC;
    cfg->afe_ringbuf_size = 20;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_config_print(cfg);                             // the effective config on the serial log: what a mode really changes
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
    ESP_LOGI(TAG, "AFE %s %s: %d samples a feed, vad %s, aec filter %d, reference delay %d ms", BOARD_AFE_FORMAT, settings.afe_mode ? "high-perf" : "low-cost", feed_chunk,
             vad_model ? "model" : "webrtc", settings.aec_filter, settings.aec_delay_ms);
    return true;
}

static int32_t q15_of_percent(int level)
{
    // A straight line to 1.6x at the top (Elecrow's own ceiling): her sentences are not full
    // scale, and the little speaker on a cord needs the room. What clips, clips at the sample.
    return (int32_t)(level / 100.0f * 1.6f * 32768);
}

void audio_start(void)
{
    speaker = xSemaphoreCreateMutex();
    if (!board_audio()->rx || !board_audio()->tx) { ESP_LOGE(TAG, "no sound: the board did not come up"); return; }
    ref = heap_caps_calloc(REF_RING, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    raw = heap_caps_malloc(RAW_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!ref || !raw) { ESP_LOGE(TAG, "no memory for the reference"); return; }
    delay_samples = settings.aec_delay_ms * AUDIO_RATE / 1000;
    aec_on = settings.aec;
    gain_q15 = q15_of_percent(settings.volume);
    spk_rate = settings.speaker_rate;
    mic_q15 = (int32_t)(powf(10, settings.mic_gain_x2 / 40.0f) * 32768);
    if (!afe_init()) { ESP_LOGE(TAG, "no sound: the AFE did not start"); return; }
    ok = true;
    task_in_psram(feed_task, "afe-feed", 8192, 8, 0);
    task_in_psram(fetch_task, "afe-fetch", 12288, 7, 1);     // the wake word network runs on it
}

void audio_stop(void)
{
    halt = true;
    board_amplifier(false);
}

bool audio_ok(void) { return ok; }

void audio_suspend(bool on)
{
    if (!ok) return;
    board_audio_t *b = board_audio();
    if (on) {
        suspended = true;
        vTaskDelay(pdMS_TO_TICKS(30));                 // a read in flight ends
        i2s_channel_disable(b->rx);
        i2s_channel_disable(b->tx);
    } else {
        i2s_channel_enable(b->rx);
        i2s_channel_enable(b->tx);
        suspended = false;
    }
}

void audio_set_mic_gain(float db)
{
    int x2 = (int)(db * 2.0f + 0.5f);
    x2 = x2 < 0 ? 0 : x2 > MIC_GAIN_X2_MAX ? MIC_GAIN_X2_MAX : x2;
    settings.mic_gain_x2 = x2;
    settings_save();
    mic_q15 = (int32_t)(powf(10, x2 / 40.0f) * 32768);
}

float audio_mic_gain(void) { return settings.mic_gain_x2 / 2.0f; }

void audio_set_volume(int level)
{
    level = level < 0 ? 0 : level > 100 ? 100 : level;
    settings.volume = level;
    settings_save_soon();
    gain_q15 = q15_of_percent(level);
}

int audio_volume(void) { return settings.volume; }

void audio_set_mute(bool on)
{
    muted = on;
    board_amplifier(!on && !power_doubt());        // the dead-battery guard's try keeps the amplifier off for its first two minutes; power.c turns it on
    if (settings.mute != on) { settings.mute = on; settings_save_soon(); }
}
bool audio_muted(void) { return muted; }
void audio_set_sink(audio_sink_fn fn) { sink = fn; }

bool audio_set_speaker_rate(int rate)
{
    if (rate != 16000 && rate != 24000) return false;
    if (rate == spk_rate) return true;
    if (!audio_take_speaker(pdMS_TO_TICKS(3000))) return false;
    bool ok = board_speaker_rate(rate);
    if (ok) { spk_rate = rate; settings.speaker_rate = rate; settings_save_soon(); stream_t0_us = 0; spk_first_us = 0; }   // the speaker's rate is measured afresh
    audio_give_speaker();
    ESP_LOGI(TAG, "the speaker runs at %d Hz now (%s)", spk_rate, ok ? "set" : "NOT set");
    return ok;
}

void audio_aec_set_delay_ms(int ms)
{
    ms = ms < -MIC_HOLD_MAX_MS ? -MIC_HOLD_MAX_MS : ms > 500 ? 500 : ms;   // negative: the MIC is held that long instead (more lead for the canceller)
    settings.aec_delay_ms = ms;
    settings_save_soon();
    delay_samples = ms * AUDIO_RATE / 1000;
}
int audio_aec_delay_ms(void) { return settings.aec_delay_ms; }
void audio_aec_enable(bool on) { aec_on = on; if (settings.aec != on) { settings.aec = on; settings_save_soon(); } }
bool audio_aec_enabled(void) { return aec_on; }

void audio_stats(float *rms, int *speech_ms)
{
    *rms = out_rms;
    int64_t now = esp_timer_get_time();
    int n = 0;
    for (int i = 0; i < 64; i++) if (speech_log[i] && now - speech_log[i] < 2000000) n++;
    *speech_ms = n * feed_chunk * 1000 / AUDIO_RATE;       // each chunk the VAD called speech, in the last 2 s
}

// --- the speaker -------------------------------------------------------------------

bool audio_take_speaker(TickType_t wait)
{
    if (!ok || xSemaphoreTake(speaker, wait) != pdTRUE) return false;
    board_speaker_check();
    return true;
}
void audio_give_speaker(void) { xSemaphoreGive(speaker); }
int audio_speaker_check(void)
{
    // The pendant's check only reads and rewrites the expander: harmless under her voice, and the watch task's
    // ui_heal after a reset rail must not wait for a reply to end (0.6.1 ran it every 5 s; scout 2026-11-12).
    // The speaker's check reopens a codec, so ITS audio_speaker_check keeps the speaker-held gate.
    return ok ? board_speaker_check() : 0;
}

// Mono in, at the volume, out as stereo frames with both slots alike; what went out
// goes into the reference ring as it was sent.
void audio_play(const int16_t *pcm, size_t samples, int channels)
{
    if (!ok || suspended) return;
    int16_t mono[256], out[512];
    size_t step = channels == 2 ? 512 : 256;
    for (size_t at = 0; at < samples; at += step) {
        size_t n = samples - at < step ? samples - at : step;
        size_t frames = channels == 2 ? n / 2 : n;
        for (size_t i = 0; i < frames; i++) {
            int32_t v = channels == 2 ? (pcm[at + 2 * i] + pcm[at + 2 * i + 1]) / 2 : pcm[at + i];
            v = (v * gain_q15) >> 15;
            mono[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
            out[2 * i] = out[2 * i + 1] = mono[i];
        }
        ref_push(mono, frames);
        size_t written = 0;
        i2s_channel_write(board_audio()->tx, out, frames * 2 * sizeof(int16_t), &written, 1000);
    }
}

void audio_play_end(void)
{
    static const int16_t quiet[SPEAKER_RATE / 20];   // 50 ms of silence at 24 kHz (75 at 16) flushes the DMA cleanly
    audio_play(quiet, sizeof quiet / sizeof quiet[0], 1);
}

void audio_tone(int hz, int ms)
{
    if (!audio_take_speaker(pdMS_TO_TICKS(3000))) return;
    int16_t buf[480];
    size_t total = (size_t)spk_rate * ms / 1000;
    for (size_t at = 0; at < total; at += 480) {
        size_t n = total - at < 480 ? total - at : 480;
        for (size_t i = 0; i < n; i++) {
            float edge = fminf(1.0f, fminf((at + i) / 600.0f, (total - at - i) / 600.0f));   // no click
            buf[i] = (int16_t)(8000 * edge * sinf(2 * M_PI * hz * (at + i) / spk_rate));
        }
        audio_play(buf, n, 1);
    }
    audio_play_end();
    audio_give_speaker();
}

// --- the measurements ------------------------------------------------------------------

static float listen_out(int ms)
{
    out_sum = 0;
    out_count = 0;
    out_measuring = true;
    vTaskDelay(pdMS_TO_TICKS(ms));
    out_measuring = false;
    return out_count ? sqrtf((float)(out_sum / out_count)) : 0;
}

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
    t->out_quiet = listen_out(400);
    xTaskCreate(tone_task, "tone", 4096, NULL, 5, NULL);
    vTaskDelay(pdMS_TO_TICKS(150));
    listen_raw(300, t->slot_rms);
    t->tone_rms = t->slot_rms[0];
    t->out_tone = listen_out(400);                 // the second half of the 900 ms tone: the canceller has had 450 ms to adapt (the front end lags the mic by its own chunking)
    vTaskDelay(pdMS_TO_TICKS(200));
}

// A 2 kHz burst of 60 ms, with the raw mic recorded from 200 ms before it to 800 ms after.
// The burst's first sample is marked with the read counter at its write; the mic shows it
// `delay` later, found as the first 10 ms window well above the room.
bool audio_aec_measure(int *delay_ms, float *ratio)
{
    *delay_ms = -1;
    *ratio = 0;
    if (!ok || suspended || !audio_take_speaker(pdMS_TO_TICKS(2000))) return false;
    raw_have = 0;
    raw_start_r = ref_r;
    raw_on = true;
    vTaskDelay(pdMS_TO_TICKS(200));
    int16_t buf[1440];                                     // 60 ms at the speaker's rate
    int nb = spk_rate * 60 / 1000;
    for (int i = 0; i < nb; i++) {
        float edge = fminf(1.0f, fminf(i / (nb / 12.0f), (nb - i) / (nb / 12.0f)));
        buf[i] = (int16_t)(12000 * edge * sinf(2 * M_PI * 2000 * i / (float)spk_rate));
    }
    burst_mark = true;
    audio_play(buf, nb, 1);
    audio_play_end();
    vTaskDelay(pdMS_TO_TICKS(800));
    raw_on = false;
    audio_give_speaker();
    size_t have = raw_have;
    const size_t win = AUDIO_RATE / 100;                   // 10 ms
    if (have < win * 30) return false;
    // the room: the first 150 ms; the burst: the first window at 3x the room and above a floor
    double room = 0;
    for (size_t i = 0; i < win * 15; i++) room += (double)raw[i] * raw[i];
    float room_rms = sqrt(room / (win * 15)) + 1;
    float best = 0;
    size_t found = 0;
    for (size_t at = win * 15; at + win <= have; at += win) {
        double e = 0;
        for (size_t i = 0; i < win; i++) e += (double)raw[at + i] * raw[at + i];
        float rms = sqrt(e / win);
        if (rms > best) best = rms;
        if (!found && rms > room_rms * 3 && rms > 60) found = at;
    }
    *ratio = best / room_rms;
    if (!found) return true;                               // measured, nothing heard: the ratio says so
    uint32_t mic_clock = raw_start_r + found;              // the mic sample where the burst began
    *delay_ms = (int)((int32_t)(mic_clock - burst_at) * 1000 / AUDIO_RATE);
    ESP_LOGI(TAG, "burst back after %d ms, %.0fx the room (the reference is read %d ms after the write)", *delay_ms, *ratio, settings.aec_delay_ms);
    return true;
}
