// avatar.c - see avatar.h.
#include "avatar.h"

#include <stdatomic.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "adpcm.h"
#include "audio.h"
#include "light.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "avatar";

#define TICKS_A_SECOND 50              // audio fed 20 ms at a time, at whatever rate: the clock's tick
// The amplifier's DMA ring, kept full by the feeder, is this far ahead of the air (80 ms at 24 kHz on the pendant)
#define LEAD_MS(sr) ((int)((int64_t)BOARD_SPK_DESC_NUM * BOARD_SPK_DESC_FRAMES * 1000 / (sr)))
#define LATE_MS 150                    // a region this far behind its moment goes into her face without a paint of its own; a key always paints
#define GATHER_US 1500000              // the start waits this long at most for av_buffer packets
#define IDLE_TICK_MS 20                // a packet without audio is paced on the wall at this tick

typedef struct { int t_ms, x, y, w, h; const uint8_t *jpeg; size_t n; bool key; } av_frame_t;
typedef struct {
    uint8_t *buf;                      // the packet as it arrived, PSRAM; freed when played or dropped
    int16_t *decoded;                  // her voice made plain from IMA ADPCM on arrival, PSRAM; NULL for PCM
    const int16_t *pcm;                // into buf
    size_t samples;
    int sr, nframes, ms;               // ms: the packet's span, the clock when there is no audio
    av_frame_t frames[AV_FRAMES_MAX];  // into buf
    unsigned gen;                      // a clear bumps gen: a packet taken before is dropped unplayed
    _Atomic int holds;                 // the feeder and the painter: the last to finish frees it
} av_packet_t;

static QueueHandle_t queue;
// The painter: the audio feeder never paints. A key frame's decode and paint (~80 ms) between
// two 20 ms chunks starved the amplifier's 40 ms DMA: her voice skipped at every key (Krem,
// 2026-11-03). Due frames go to the painter task; late ones it skips; the packet's buffer is
// freed by whoever is done with it last.
#define PAINT_QUEUE 32                 // several boxes a frame: 8 dropped jobs once a frame went as four
typedef struct { av_packet_t *pkt; int index; } paint_job_t;   // index -1: the packet's audio is done
static QueueHandle_t paints;
static volatile int clock_ms;          // the air's moment in the packet being fed, for the painter's lateness
static _Atomic unsigned gen;
static volatile int64_t last_packet_us, last_paint_us;
static volatile bool playing, voice;   // voice: the speaker is held for her stream
static _Atomic int voice_waiting;      // packets with her voice queued, not yet in the feeder's hand: her turn, before the speaker is hers
static int64_t idle_end_us;            // her idle's running deadline: each packet starts where the last was due to end, so the tick's overshoot never piles up
static avatar_stats_t st;
static bool started;                   // avatar_start made the queues and the tasks
static uint8_t *scratch;               // one decoded region, up to the whole face
static _Atomic bool need_key;          // a frame did not paint: told once in the next packet's answer

static int number(const cJSON *o, const char *key) { const cJSON *v = cJSON_GetObjectItem(o, key); return cJSON_IsNumber(v) ? (int)v->valuedouble : -1; }
#define V1_SIDE ((BOARD_W < BOARD_H ? BOARD_W : BOARD_H) / 16 * 16)   // the square an older Sapphire sizes frames to

static uint32_t be(const uint8_t *p, int n) { uint32_t v = 0; while (n--) v = v << 8 | *p++; return v; }

// One frame entry checked against the face box and the body: false = the packet is not one.
static bool frame_ok(const av_frame_t *q, size_t at, size_t len)
{
    // written so nothing can wrap: a v1 x of 2^31-16 made x + w negative and passed (scout 2026-11-12)
    return q->t_ms >= 0 && q->x >= 0 && q->y >= 0 && q->w > 0 && q->h > 0 && !(q->w & 15) && !(q->h & 15)
        && q->w <= AV_W && q->x <= AV_W - q->w && q->h <= AV_H && q->y <= AV_H - q->h
        && q->n > 0 && at <= len && q->n <= len - at;
}

static void packet_free(av_packet_t *p) { free(p->decoded); free(p->buf); free(p); }

int avatar_take(uint8_t *body, size_t len, int *queued, bool refuse_voice)
{
    if (queued) *queued = queue ? (int)uxQueueMessagesWaiting(queue) : 0;
    if (!queue || len < 8) return 400;
    size_t hl = be(body, 4);
    if (hl > 8192 || 4 + hl > len || ((4 + hl) & 1)) return 400;        // the head is padded to an even length: the audio's samples are aligned
    cJSON *head = cJSON_ParseWithLength((const char *)body + 4, hl);
    if (!head) return 400;
    // Built in place, in PSRAM: 96 frame entries are 3 KB, more than the door task's stack should hold
    av_packet_t *p = heap_caps_calloc(1, sizeof *p, MALLOC_CAP_SPIRAM);
    if (!p) { cJSON_Delete(head); return 503; }
    p->buf = body;
    p->gen = atomic_load(&gen);
    int v = number(head, "v"), audio_n = number(head, "audio"), nf = number(head, "nf");
    int samples = number(head, "samples"), block = number(head, "block");
    const cJSON *codec = cJSON_GetObjectItem(head, "codec");
    bool ima = cJSON_IsString(codec) && !strcmp(codec->valuestring, "ima_adpcm");
    bool known = !cJSON_IsString(codec) || ima || !strcmp(codec->valuestring, "pcm");
    p->sr = number(head, "sr");
    p->ms = number(head, "ms");
    size_t at = 4 + hl;
    int code = 0;
    if (v == 2) {                                                       // the frame table follows the head
        if (nf < 0 || nf > AV_FRAMES_MAX || at + (size_t)nf * 16 > len) code = 400;
        else {
            const uint8_t *e = body + at;
            at += (size_t)nf * 16;
            size_t data = at + (audio_n > 0 ? (size_t)audio_n : 0);
            for (int i = 0; i < nf && !code; i++, e += 16) {
                av_frame_t *q = &p->frames[i];
                q->t_ms = (int)be(e, 2); q->x = (int)be(e + 2, 2); q->y = (int)be(e + 4, 2); q->w = (int)be(e + 6, 2); q->h = (int)be(e + 8, 2);
                q->key = be(e + 10, 2) & 1;
                q->n = be(e + 12, 4);
                if (!frame_ok(q, data, len)) code = 400;
                q->jpeg = body + data;
                data += q->n;
            }
            p->nframes = nf;
        }
    }
    cJSON *frames = v == 2 ? NULL : cJSON_GetObjectItem(head, "frames");
    if (!code && (audio_n < 0 || at + (size_t)audio_n > len || !known)) code = 400;
    if (!code && !ima && (audio_n & 1)) code = 400;
    if (!code) {
        const uint8_t *audio = body + at;
        if (ima) {
            size_t can = adpcm_samples((size_t)audio_n, block > 0 ? (size_t)block : 0);
            size_t n = samples >= 0 && (size_t)samples < can ? (size_t)samples : can;
            if (audio_n && !n) code = 400;
            else if (n && !(p->decoded = heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM))) code = 503;
            else if (n) n = adpcm_decode(audio, (size_t)audio_n, (size_t)block, p->decoded, n);
            p->pcm = p->decoded;
            p->samples = n;
        } else {
            p->pcm = (const int16_t *)audio;
            p->samples = (size_t)audio_n / 2;
        }
        at += (size_t)audio_n;
    }
    if (!code && v != 2) {                                              // v1: the frame list is JSON
        const cJSON *f;
        cJSON_ArrayForEach(f, frames) {
            if (p->nframes >= AV_FRAMES_MAX) { code = 400; break; }   // more than a packet may carry: refused whole, never a key silently dropped
            av_frame_t *q = &p->frames[p->nframes];
            q->t_ms = number(f, "t"); q->x = number(f, "x"); q->y = number(f, "y"); q->w = number(f, "w"); q->h = number(f, "h");
            int n = number(f, "n");
            q->n = n > 0 ? (size_t)n : 0;
            q->key = cJSON_IsTrue(cJSON_GetObjectItem(f, "key"));
            // A v1 sender is an older Sapphire (2.16.x): it knows only the glass's square, not our box, so its
            // frames are centred in the box (the pendant's 240x288: 24 px each end; the speaker's 320x240: 40
            // each side). Today's hub sends v2, sized to the box: this never runs for it.
            if (q->x < 0 || q->y < 0 || q->x > V1_SIDE || q->y > V1_SIDE) { code = 400; break; }
            q->x += (AV_W - V1_SIDE) / 2;
            q->y += (AV_H - V1_SIDE) / 2;
            if (!frame_ok(q, at, len)) { code = 400; break; }
            q->jpeg = body + at;
            at += q->n;
            p->nframes++;
        }
    }
    cJSON_Delete(head);                                             // read: the frames above point into the body, not into it
    // An older Sapphire sends the stream's voice at 24 kHz to every board (it has no fitter): a board that plays
    // 16 kHz (the speaker) takes it down 3:2 here rather than refuse it, which silenced its own replies there.
    // Today's hub fits the voice to the board's rate first: this never runs for it.
    if (!code && p->samples && p->sr * 2 == audio_speaker_rate() * 3) {
        size_t out_n = p->samples / 3 * 2;
        int16_t *out = out_n ? heap_caps_malloc(out_n * 2, MALLOC_CAP_SPIRAM) : NULL;
        if (!out) code = 503;
        else {
            for (size_t i = 0, k = 0; k < out_n; i += 3, k += 2) {        // out_n is a whole number of 3-sample groups, so i + 2 stays inside
                out[k] = p->pcm[i];                                         // at 0
                out[k + 1] = (int16_t)(((int)p->pcm[i + 1] + p->pcm[i + 2]) / 2);   // at 1.5
            }
            free(p->decoded);
            p->decoded = out;
            p->pcm = out;
            p->samples = out_n;
            p->sr = audio_speaker_rate();
        }
    }
    if (!code && p->samples && p->sr != audio_speaker_rate()) code = 415;
    if (!code && p->samples && refuse_voice) code = 204;               // stopped, or a question is being heard: her voice is not played over it (the sentence door does the same, server.c)
    if (!code && !p->samples && p->ms <= 0) code = 400;                // no audio: the packet states its span
    if (!code && p->ms > 10000) code = 400;                            // a span past ten seconds would pin the feeder (a packet is about one)
    if (code) { free(p->decoded); free(p); return code; }           // the caller still owns `body`
    atomic_store(&p->holds, 2);                              // the feeder and the painter
    if (p->samples) atomic_fetch_add(&voice_waiting, 1);                                  // counted before it can be taken off the queue
    av_packet_t *pp = p;
    if (xQueueSend(queue, &pp, 0) != pdTRUE) { if (p->samples) atomic_fetch_sub(&voice_waiting, 1); free(p->decoded); free(p); return 429; }
    last_packet_us = esp_timer_get_time();
    st.packets++;
    if (queued) *queued = (int)uxQueueMessagesWaiting(queue);
    return 0;
}

void avatar_clear(void)
{
    atomic_fetch_add(&gen, 1);
    av_packet_t *p;
    while (queue && xQueueReceive(queue, &p, 0) == pdTRUE) { if (p->samples) atomic_fetch_sub(&voice_waiting, 1); packet_free(p); }
}

bool avatar_live(void) { return playing || (last_packet_us && esp_timer_get_time() - last_packet_us < 3000000); }
bool avatar_voice(void) { return voice || atomic_load(&voice_waiting) > 0; }   // playing, or queued to: her turn either way (a press over a packet still in the queue was read as "listen", scouts 2026-11-05)
int avatar_scratch_mod16(void) { return (int)((uintptr_t)scratch & 15); }
bool avatar_need_key(void) { return atomic_exchange(&need_key, false); }

void avatar_stats(avatar_stats_t *out)
{
    *out = st;
    out->queued = queue ? (int)uxQueueMessagesWaiting(queue) : 0;
    out->playing = playing;
    out->voice = voice;
}

static bool decode_region(const av_frame_t *f)
{
    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    jpeg_dec_io_t io = {.inbuf = (uint8_t *)f->jpeg, .inbuf_len = (int)f->n, .outbuf = scratch};
    jpeg_dec_header_info_t info;
    int64_t t0 = esp_timer_get_time();
    int step = 0, err = 0;                     // which step failed, and the decoder's word on it
    memset(&info, 0, sizeof info);
    bool ok = (++step, (err = jpeg_dec_open(&cfg, &dec)) == JPEG_ERR_OK) && (++step, (err = jpeg_dec_parse_header(dec, &io, &info)) == JPEG_ERR_OK)
              && (++step, info.width == f->w && info.height == f->h) && (++step, (err = jpeg_dec_process(dec, &io)) == JPEG_ERR_OK);
    if (dec) jpeg_dec_close(dec);
    st.decode_ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (!ok) { st.fail_step = step; st.fail_w = f->w; st.fail_h = f->h; st.fail_n = (int)f->n; st.hdr_w = (int)info.width; st.hdr_h = (int)info.height; st.err = err; }
    if (!ok && st.decode_fail++ % 25 == 0)
        ESP_LOGW(TAG, "a frame did not decode at step %d (%s): %dx%d at %d,%d, %u bytes, header says %dx%d", step,
                 step == 1 ? "open" : step == 2 ? "header" : step == 3 ? "size" : "process", f->w, f->h, f->x, f->y, (unsigned)f->n,
                 step >= 3 ? (int)info.width : -1, step >= 3 ? (int)info.height : -1);
    return ok;
}

static void paint(const av_frame_t *f, bool with_voice, bool show, bool moment);

static void packet_done(av_packet_t *pkt)   // the audio fed and the painter past its last frame: the buffer goes
{
    if (atomic_fetch_sub(&pkt->holds, 1) == 1) packet_free(pkt);
}

static void painter_task(void *arg)
{
    paint_job_t j;
    for (;;) {
        if (xQueueReceive(paints, &j, portMAX_DELAY) != pdTRUE) continue;
        if (j.index < 0) { ui_av_flush(); packet_done(j.pkt); continue; }   // a late region at a stream's end is shown, not left unseen
        const av_frame_t *f = &j.pkt->frames[j.index];
        if (j.pkt->gen != atomic_load(&gen)) continue;
        bool late = !f->key && clock_ms - f->t_ms > LATE_MS;
        if (late) st.late++;                     // into her face all the same, to the panel with the next: skipping it left a stale patch until a key
        // A frame may go as several boxes with one moment: frames and gap count moments, not boxes
        // (0.6.0 counted each box: gap_ms read 36 for 12 frames a second, 2026-11-12)
        static const av_packet_t *last_pkt;
        static int last_t = -1;
        bool moment = j.pkt != last_pkt || f->t_ms != last_t;
        last_pkt = j.pkt;
        last_t = f->t_ms;
        paint(f, j.pkt->samples > 0, !late, moment);
    }
}

static void paint(const av_frame_t *f, bool with_voice, bool show, bool moment)
{
    int64_t t0 = esp_timer_get_time();
    if (!decode_region(f)) atomic_store(&need_key, true);       // her face here is not the one sent: the sender's next frame is a key
    else if (!ui_av_frame(scratch, f->x, f->y, f->w, f->h, f->key, with_voice, show)) {
        atomic_store(&need_key, true);
        if (st.paint_fail++ % 25 == 0) ESP_LOGW(TAG, "a frame decoded but was not painted: %dx%d at %d,%d (%s)", f->w, f->h, f->x, f->y, ui_av_refusal());
    } else if (show && moment) {
        st.frames++;
        if (last_paint_us) { int gap = (int)((t0 - last_paint_us) / 1000); st.gap_ms = st.gap_ms ? (st.gap_ms * 7 + gap) / 8 : gap; }
        last_paint_us = t0;
    }
    st.paint_ms = (int)((esp_timer_get_time() - t0) / 1000);
}

// The audio is the clock: every tick fed, the frames whose moment the air has reached are
// painted (the DMA is LEAD_MS ahead of the air). A region far behind its moment goes into her
// face unpainted; a key always paints: it is what the regions after it are drawn on.
static void hand(av_packet_t *p, int index)
{
    paint_job_t j = {.pkt = p, .index = index};
    if (xQueueSend(paints, &j, index < 0 ? portMAX_DELAY : 0) != pdTRUE) { st.late++; atomic_store(&need_key, true); }   // the painter is behind: this one is lost, the audio is not; a key mends it
}

// The frames now due, from `next`: when a key is among them, what comes before it is covered by
// it and never decoded (the painter fell behind on a run of keys and her face came in bursts).
static int hand_due(av_packet_t *p, int next, int clock)
{
    int end = next, key = -1;
    while (end < p->nframes && p->frames[end].t_ms <= clock) { if (p->frames[end].key) key = end; end++; }
    if (key > next) { st.skipped += key - next; next = key; }
    while (next < end) hand(p, next++);
    return next;
}

static void play_packet(av_packet_t *p)
{
    int next = 0;
    if (p->samples) {                                    // her voice is the clock
        idle_end_us = 0;
        size_t fed = 0, chunk = (size_t)p->sr / TICKS_A_SECOND;
        while (fed < p->samples) {
            if (p->gen != atomic_load(&gen)) break;
            clock_ms = (int)(fed * 1000 / (size_t)p->sr) + LEAD_MS(p->sr);
            next = hand_due(p, next, clock_ms);
            size_t n = p->samples - fed < chunk ? p->samples - fed : chunk;
            audio_play(p->pcm + fed, n, 1);
            fed += n;
        }
    } else {                                             // her idle: the packet's own span, on the wall
        // From where the last idle packet was due to end, when that was a moment ago: paced from each
        // packet's own start, every one overshot by up to a tick and idle piled up ahead of her voice
        // (scouts, 2026-11-05). And a packet with her voice behind this one is not made to wait: the
        // idle yields at the next tick, its frames go now, the voice packet's key repaints.
        int64_t now = esp_timer_get_time();
        int64_t t0 = idle_end_us && now - idle_end_us < 200000 ? idle_end_us : now;
        idle_end_us = t0 + (int64_t)p->ms * 1000;
        for (;;) {
            if (p->gen != atomic_load(&gen) || atomic_load(&voice_waiting) > 0) { next = p->nframes; break; }   // yields: her idle's rest is stale under the voice packet's key (it flooded the painter, scout)
            now = esp_timer_get_time();
            clock_ms = (int)((now - t0) / 1000);
            next = hand_due(p, next, clock_ms);
            if (now >= idle_end_us) break;
            vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
        }
    }
    while (next < p->nframes) hand(p, next++);           // due after the packet's end: now
    hand(p, -1);                                         // the painter frees it after its last frame
}

// Her voice's end, from this task only (the speaker's mutex is given by its holder, never another
// task): the cue goes first, then the speaker, so the player that takes it next and paints
// "speaking" is not wiped to idle a moment later by this one (two cores; scouts 2026-11-05).
static void voice_end(void)
{
    audio_play_end();
    voice = false;
    light_speech_done();
    audio_give_speaker();
}

static void av_task(void *arg)
{
    av_packet_t *p;
    int64_t waited_us = 0;                                   // how long the last packet was waited for
    for (;;) {
        if (xQueueReceive(queue, &p, portMAX_DELAY) != pdTRUE) continue;
        if (p->samples) atomic_fetch_sub(&voice_waiting, 1);
        bool fresh = p->gen == atomic_load(&gen);
        if (fresh && p->samples && !voice) {
            // Her voice starts, after nothing or after her idle: let av_buffer packets gather (the
            // server renders unevenly), take the speaker, open her tile. A stop that lands during
            // either wait is honoured: the packet is dropped unstarted (her tile opened and the ring
            // said "speaking" for a reply already stopped, scouts 2026-11-05).
            int64_t t0 = esp_timer_get_time();
            while ((int)uxQueueMessagesWaiting(queue) + 1 < settings.av_buffer && p->gen == atomic_load(&gen) && esp_timer_get_time() - t0 < GATHER_US) vTaskDelay(pdMS_TO_TICKS(20));
            fresh = p->gen == atomic_load(&gen);
            if (fresh && !audio_take_speaker(pdMS_TO_TICKS(3000))) { ESP_LOGW(TAG, "the speaker is held: a packet dropped"); fresh = false; }
            else if (fresh && p->gen != atomic_load(&gen)) { audio_give_speaker(); fresh = false; }
            if (fresh) {
                voice = true;
                ui_av_show();
                light_cue("speaking");                       // the halo and the ring: her voice is on, from her stream
                ESP_LOGI(TAG, "her voice starts: %d packet(s) in hand", (int)uxQueueMessagesWaiting(queue) + 1);
            }
        } else if (fresh && !p->samples && voice) {
            voice_end();                                     // her voice ended and her idle follows: the speaker goes back
        } else if (voice && p->samples && waited_us > 50000) st.underruns++;   // her voice had a hole: this packet came late
        if (!playing) { playing = true; last_paint_us = 0; ESP_LOGI(TAG, "her stream starts with %s", p->samples ? "her voice" : "her idle"); }
        if (fresh) play_packet(p);
        else hand(p, -1);                                    // cleared or not started: the painter still owns a hold
        bool stale = p->gen != atomic_load(&gen);            // read BEFORE the packet may be freed (it was read after packet_done: a use after free, scouts 2026-11-05)
        packet_done(p);                                      // the feeder's hold
        if (voice && stale) voice_end();                     // stopped mid-voice: the speaker goes back NOW, not after the 1.5 s wait, so the next press listens
        int64_t w0 = esp_timer_get_time();
        unsigned g = atomic_load(&gen);
        av_packet_t *peek;
        bool more = false;
        while (!more && esp_timer_get_time() - w0 < 1500000) {   // 1.5 s for the next packet, in slices: a stop in the wait gives the speaker back now, not at the end of it
            more = xQueuePeek(queue, &peek, pdMS_TO_TICKS(100)) == pdTRUE;
            if (voice && atomic_load(&gen) != g) voice_end();
        }
        if (more) { waited_us = esp_timer_get_time() - w0; continue; }
        if (voice) voice_end();                              // the stream is over: nothing more for 1.5 s
        playing = false;
        ui_av_end();
        ESP_LOGI(TAG, "her stream ends: %d packets, %d frames, %d late, %d holes", st.packets, st.frames, st.late, st.underruns);
    }
}

void avatar_start(void)
{
    // The decoder's output buffer MUST be 16-byte aligned (esp_jpeg_dec.h): a plain malloc gave an
    // address that happened to be aligned for hours, then not after a cold boot, and every frame
    // failed to decode with nothing said (0.3.0-0.3.1, 2026-11-03). Aligned, and health shows it.
    scratch = heap_caps_aligned_alloc(16, (size_t)AV_W * AV_H * 2, MALLOC_CAP_SPIRAM);
    queue = xQueueCreate(AV_QUEUE, sizeof(av_packet_t *));
    paints = xQueueCreate(PAINT_QUEUE, sizeof(paint_job_t));
    if (!scratch || !queue || !paints) { ESP_LOGE(TAG, "no room for her stream"); return; }
    task_in_psram(av_task, "avatar", 6144, 6, 0);           // feeds her voice: never a paint; above the server task (5) that takes the packets
    task_in_psram(painter_task, "painter", 8192, 3, 1);     // decodes and paints, on the glass core, UNDER the LVGL task (4): a tap and a state change before a cosmetic frame
    started = true;
}

bool avatar_ok(void) { return started; }   // health screen.av: true only when the stream can be taken (it said true with no queue, scouts 2026-11-05)
