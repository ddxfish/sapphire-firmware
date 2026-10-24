// server.c - the board's own tiny HTTP server (see server.h). Sapphire is the
// client: she learns the board's address when it registers, polls /health to
// see it has a speaker, and POSTs each reply sentence to /audio/speak as a
// 16 kHz mono PCM WAV. We stream that body straight to the ES8311 DAC in small
// internal-RAM chunks - esp_codec_dev_write blocks at real time, so TCP flow
// control paces the upload and no whole-sentence buffer is needed (this board
// has no PSRAM). Plain HTTP on the LAN; the bearer key gates every route.
#include "server.h"

#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "board.h"
#include "ir.h"
#include "light.h"
#include "settings.h"
#include "wav.h"

static const char *TAG = "server";
#define HALT_MS 3000                       // after /audio/stop, the rest of the reply is refused this long
// Sapphire sends her reply as IMA ADPCM WAV (4:1), decoded here to 16 kHz PCM.
// Plain 16-bit PCM is 256 kbps and a congested 2.4 GHz link could not hold it
// (the drops were WiFi receive throughput). ADPCM is ~64 kbps, well under the
// link's floor, and keeps the full 16 kHz band so her voice stays clear.
// /health states the codec; on_speak still plays plain PCM too, so an older
// sender (or the moment before Sapphire re-reads /health) just works.
#define RING_BYTES (48 * 1024)             // The ring holds COMPRESSED audio (ADPCM frames), not decoded PCM, so
                                           // 48 KB is ~6 s of cushion (4x) - enough to coast over a 1-2 s WiFi
                                           // dip. 48 KB is the ceiling (no PSRAM; the ring competes with the
                                           // voice/events machinery - 96 KB starved PTT + events). Do not grow it.
#define PRIME_BYTES (4 * 1024)             // ~0.5 s of ADPCM: the cushion built before playback starts
static volatile bool cut;                  // /audio/stop while a sentence plays
static volatile bool priming;              // true: hold the player while the cushion builds (no underrun at the start)
static volatile int diag_underruns;        // DIAG: times the player faded out (ring ran dry) - delivery-vs-generation probe
static int64_t diag_last_end;              // DIAG: us when the previous sentence's POST finished
static volatile int64_t halt_until;
static uint8_t frame[1030];                 // on_speak assembles one framed unit here: [type][len16][payload]
static StreamBufferHandle_t ring;          // recv task fills it with frames, the player drains + decodes them

static int ima_decode_block(const uint8_t *blk, int len, int16_t *out);   // defined below

// Read exactly n bytes of a frame from the ring: the first byte waits first_ms
// (0 = the ring is empty, a reply gap or its end), the rest block (the payload
// is written right behind its header, so it is already on its way). Returns n,
// or 0 when the ring was empty at a frame boundary.
static int ring_read(uint8_t *buf, int n, int first_ms)
{
    int got = 0;
    while (got < n) {
        size_t r = xStreamBufferReceive(ring, buf + got, n - got,
                                        got == 0 ? pdMS_TO_TICKS(first_ms) : portMAX_DELAY);
        if (r == 0) { if (got == 0) return 0; continue; }
        got += (int)r;
    }
    return n;
}

// The player drains frames from the ring, decodes each, and writes it to the
// DAC at real time. The receiver runs ahead, up to RING_BYTES of COMPRESSED
// audio (~6 s), and `priming` holds the player until a PRIME_BYTES cushion is
// built - so a WiFi dip shorter than the cushion is coasted over instead of
// starving the DAC. Fade over ~5 ms at the edges of sound, so a start, an end,
// or an underrun is a soft dip instead of a hard click/cut.
#define FADE 80

static void play_task(void *arg)
{
    static uint8_t blk[1024];                     // one frame's payload (ADPCM block, or PCM)
    static int16_t out[2100];                     // its decoded PCM16
    bool sounding = false;
    int16_t last = 0;                             // last sample played: fade down from it on an underrun
    for (;;) {
        if (priming) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        uint8_t hdr[3];
        if (ring_read(hdr, 3, 100) != 3) {        // ring empty at a frame boundary: reply end or an underrun
            if (sounding) {
                int16_t tail[FADE];
                for (int i = 0; i < FADE; i++) tail[i] = (int16_t)((int)last * (FADE - 1 - i) / FADE);
                board_speaker_write(tail, FADE);
                sounding = false; last = 0;
                diag_underruns++;                 // DIAG
            }
            continue;
        }
        int type = hdr[0], len = hdr[1] | hdr[2] << 8;
        if (len <= 0 || len > (int)sizeof blk || ring_read(blk, len, 100) != len) continue;
        int samples = type == 1 ? ima_decode_block(blk, len, out)
                                : (memcpy(out, blk, len), len / 2);     // type 0 = plain PCM16
        if (samples <= 0) continue;
        if (!sounding) {                          // resuming: fade in from silence (no click)
            int r = samples < FADE ? samples : FADE;
            for (int i = 0; i < r; i++) out[i] = (int16_t)((int)out[i] * i / FADE);
            sounding = true;
        }
        board_speaker_write(out, samples);
        last = out[samples - 1];
    }
}

static void audio_start(void)
{
    static bool started;
    if (started) return;
    ring = xStreamBufferCreate(RING_BYTES, 2);
    if (!ring) { ESP_LOGW(TAG, "no RAM for the play ring"); return; }
    started = true;
    xTaskCreatePinnedToCore(play_task, "play", 4096, NULL, 6, NULL, 1);   // core 1: off WiFi/httpd (core 0)
}

// --- helpers ---
static bool key_same(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned d = (unsigned)(la ^ lb);
    for (size_t i = 0; i < la; i++) d |= (unsigned)(a[i] ^ b[i < lb ? i : 0]);
    return d == 0;
}

static esp_err_t refuse(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[160];
    snprintf(body, sizeof body, "{\"ok\": false, \"error\": \"%s\"}", msg);
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s ? s : "{}");
    cJSON_free(s);
    cJSON_Delete(j);
    return ESP_OK;
}

static bool let_in(httpd_req_t *req)
{
    char auth[SETTING_MAX + 8] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof auth) != ESP_OK
        || strncmp(auth, "Bearer ", 7) || !settings.key[0] || !key_same(auth + 7, settings.key)) {
        refuse(req, "401 Unauthorized", "wrong key");
        return false;
    }
    return true;
}

static int recv_all(httpd_req_t *req, void *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, (char *)buf + got, len - got);
        if (n <= 0) return n < 0 ? n : got;
        got += n;
    }
    return got;
}

// --- /health: say what the board is and can do (Sapphire reads this) ---
static esp_err_t on_health(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *j = cJSON_CreateObject();
    cJSON *has = cJSON_AddArrayToObject(j, "has");
    cJSON_AddItemToArray(has, cJSON_CreateString("mic"));      // PTT voice (POST /voice) - must stay advertised
    cJSON_AddItemToArray(has, cJSON_CreateString("speaker"));  // TTS playback (POST /audio/speak)
    cJSON_AddItemToArray(has, cJSON_CreateString("ir"));       // IR blaster (POST /ir/blast)
    cJSON *plays = cJSON_AddObjectToObject(j, "plays");
    cJSON_AddStringToObject(plays, "type", "audio/wav");
    cJSON_AddNumberToObject(plays, "rate", AUDIO_RATE);    // 16 kHz
    cJSON_AddNumberToObject(plays, "channels", 1);
    cJSON_AddStringToObject(plays, "codec", "ima_adpcm");  // 4:1, so a congested 2.4 GHz link keeps up
    cJSON_AddStringToObject(j, "name", settings.name);
    cJSON_AddStringToObject(j, "firmware", PROJECT_VER);
    return send_json(req, j);
}

// --- IMA ADPCM decode (the codec Sapphire sends; 4:1, 4 bits/sample) ---
static const int ima_step[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
    130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
    1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
    5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767 };
static const int ima_index[16] = { -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8 };

static inline int16_t ima_nibble(int nib, int *pred, int *idx)
{
    int step = ima_step[*idx], diff = step >> 3;
    if (nib & 1) diff += step >> 2;
    if (nib & 2) diff += step >> 1;
    if (nib & 4) diff += step;
    *pred += (nib & 8) ? -diff : diff;
    if (*pred > 32767) *pred = 32767; else if (*pred < -32768) *pred = -32768;
    *idx += ima_index[nib];
    if (*idx < 0) *idx = 0; else if (*idx > 88) *idx = 88;
    return (int16_t)*pred;
}

// One mono IMA ADPCM block -> PCM16 in out[]; returns samples written. A block
// is [int16 predictor][uint8 step_index][uint8 0] then low-nibble-first codes;
// the header predictor is the block's first sample.
static int ima_decode_block(const uint8_t *blk, int len, int16_t *out)
{
    if (len < 4) return 0;
    int pred = (int16_t)(blk[0] | blk[1] << 8), idx = blk[2];
    if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
    int n = 0;
    out[n++] = (int16_t)pred;
    for (int i = 4; i < len; i++) {
        out[n++] = ima_nibble(blk[i] & 0x0f, &pred, &idx);
        out[n++] = ima_nibble(blk[i] >> 4, &pred, &idx);
    }
    return n;
}

// Feed decoded PCM16 into the ring with backpressure; clears priming once the
// cushion is built. False = the player is wedged (give up this sentence).
static bool feed_ring(const void *data, int bytes)
{
    const uint8_t *p = data;
    for (int off = 0; off < bytes && !cut; ) {
        size_t s = xStreamBufferSend(ring, p + off, bytes - off, pdMS_TO_TICKS(3000));
        if (!s) return false;
        off += (int)s;
    }
    if (priming && xStreamBufferBytesAvailable(ring) >= PRIME_BYTES) priming = false;
    return true;
}

// Read the WAV header off the request (RIFF/WAVE, then chunks up to `data`):
// *tag = 1 (PCM) or 0x11 (IMA ADPCM), *block = ADPCM block size, *len = data
// bytes. False on a malformed header. recv_all leaves the stream at the data.
static bool read_wav_head(httpd_req_t *req, int *tag, int *block, long *len)
{
    uint8_t rh[12];
    if (recv_all(req, rh, 12) != 12 || memcmp(rh, "RIFF", 4) || memcmp(rh + 8, "WAVE", 4)) return false;
    *tag = 1; *block = 0; *len = 0;
    for (int guard = 0; guard < 16; guard++) {        // a handful of chunks, then give up
        uint8_t ch[8];
        if (recv_all(req, ch, 8) != 8) return false;
        uint32_t sz = ch[4] | ch[5] << 8 | ch[6] << 16 | ch[7] << 24;
        int pad = sz & 1;
        if (!memcmp(ch, "data", 4)) { *len = sz; return true; }
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t f[40];
            int r = sz > sizeof f ? (int)sizeof f : (int)sz;
            if (recv_all(req, f, r) != r) return false;
            *tag = f[0] | f[1] << 8;
            *block = f[12] | f[13] << 8;
            sz -= r;
        }
        for (long left = (long)sz + pad; left > 0; ) {   // skip the rest of this chunk + any pad byte
            uint8_t d[32];
            int k = left > 32 ? 32 : (int)left;
            if (recv_all(req, d, k) != k) return false;
            left -= k;
        }
    }
    return false;
}

// --- /audio/speak: frame the WAV body (ADPCM or PCM) into the ring as it arrives ---
static esp_err_t on_speak(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    char ct[32] = {0};
    httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct);
    if (strncmp(ct, "audio/wav", 9) && strncmp(ct, "audio/x-wav", 11))
        return refuse(req, "415 Unsupported Media Type", "send audio/wav");
    if (esp_timer_get_time() < halt_until)
        return refuse(req, "409 Conflict", "stopped");

    cut = false;
    int tag, block;
    long data_len;
    if (!read_wav_head(req, &tag, &block, &data_len)) return refuse(req, "400 Bad Request", "bad wav");
    if (!ring) return refuse(req, "503 Service Unavailable", "no audio");

    int64_t diag_t0 = esp_timer_get_time();           // DIAG
    int diag_gap = diag_last_end ? (int)((diag_t0 - diag_last_end) / 1000) : -1;
    int diag_ur0 = diag_underruns, diag_rx = 0;
    // Rebuild the cushion only when playback has caught up - the first sentence
    // of a reply, or after an underrun. Mid-reply the ring still holds the tail
    // of the last sentence, so we keep playing without a pause and this one
    // queues behind it.
    if (xStreamBufferBytesAvailable(ring) < PRIME_BYTES) priming = true;
    light_speaking(true);

    bool ok = true;
    if (tag == 0x11 && block >= 8 && block <= 1024) {
        int have = 0;                       // bytes gathered toward the current ADPCM block
        while (data_len > 0 && !cut) {
            int want = block - have;
            if ((long)want > data_len) want = (int)data_len;
            int n = httpd_req_recv(req, (char *)frame + 3 + have, want);
            if (n <= 0) break;
            diag_rx += n; have += n; data_len -= n;
            if (have == block) {            // a full block: store it as a frame, decoded at playout
                frame[0] = 1; frame[1] = have & 0xff; frame[2] = have >> 8;
                if (!feed_ring(frame, 3 + have)) { ok = false; break; }
                have = 0;
            }
        }
        if (have >= 4 && !cut) {            // the last block is usually short
            frame[0] = 1; frame[1] = have & 0xff; frame[2] = have >> 8;
            feed_ring(frame, 3 + have);
        }
    } else {                                // plain PCM16 (an older sender, or the pre-re-poll moment)
        int rem = 0;                        // a half-sample byte carried between chunks
        while (!cut) {
            int n = httpd_req_recv(req, (char *)frame + 3 + rem, 1024 - rem);
            if (n <= 0) break;              // 0 = whole body received, <0 = error/timeout
            diag_rx += n;
            int total = rem + n, whole = (total / 2) * 2;
            if (whole > 0) {
                frame[0] = 0; frame[1] = whole & 0xff; frame[2] = whole >> 8;
                if (!feed_ring(frame, 3 + whole)) { ok = false; break; }
            }
            rem = total - whole;
            if (rem) frame[3] = frame[3 + whole];   // carry the odd byte to the front of the next payload
        }
    }
    priming = false;                        // a short sentence never reached the cushion: play what there is
    if (cut || !ok) xStreamBufferReset(ring);   // a wedge left a partial frame: clear it
    int64_t diag_t1 = esp_timer_get_time();           // DIAG: gap since last sentence, time to receive this one, bytes, and
    ESP_LOGW(TAG, "diag gap=%dms recv=%dms bytes=%d drained=%d",   // whether the ring ran dry in between (a real drop)
             diag_gap, (int)((diag_t1 - diag_t0) / 1000), diag_rx, diag_underruns - diag_ur0);
    diag_last_end = diag_t1;
    // Answer as soon as the sentence is taken in (it is in the ring), NOT after
    // it has played. Sapphire then sends the next sentence while this one is
    // still sounding, and it queues behind it in the ring - no hole at the joint
    // (the protocol's "0.7.7+ answer when taken in"). The ~1.5 s ring is the
    // cushion across sentences; play_task ends the reply cleanly when it drains.
    light_speaking(false);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    if (cut) cJSON_AddBoolToObject(j, "stopped", true);
    return send_json(req, j);
}

// --- /audio/stop: cut what is playing and refuse the rest of this reply ---
static esp_err_t on_stop(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cut = true;
    halt_until = esp_timer_get_time() + (int64_t)HALT_MS * 1000;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "stopped", true);
    return send_json(req, j);
}

// --- /ir/blast?addr=04&cmd=08: send one NEC code out the emitter ---
static esp_err_t on_ir_blast(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    char q[64], v[8];
    long addr = -1, cmd = -1;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "addr", v, sizeof v) == ESP_OK) addr = strtol(v, NULL, 16);
        if (httpd_query_key_value(q, "cmd", v, sizeof v) == ESP_OK) cmd = strtol(v, NULL, 16);
    }
    if (addr < 0 || addr > 0xffff || cmd < 0 || cmd > 0xff)
        return refuse(req, "400 Bad Request", "need addr (<=ffff) and cmd (<=ff) as hex");
    ir_send_nec((uint16_t)addr, (uint8_t)cmd);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, j);
}

void server_start(void)
{
    static httpd_handle_t server;
    if (server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32080;                 // off the default, in case anything else uses it
    cfg.max_uri_handlers = 8;
    cfg.recv_wait_timeout = 30;            // seconds a stalled stream may pause
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) != ESP_OK) { ESP_LOGE(TAG, "the server did not start"); return; }
    audio_start();                         // the play ring + player task for smooth TTS
    const httpd_uri_t routes[] = {
        {"/health",      HTTP_GET,  on_health,   NULL},
        {"/audio/speak", HTTP_POST, on_speak,    NULL},
        {"/audio/stop",  HTTP_POST, on_stop,     NULL},
        {"/ir/blast",    HTTP_POST, on_ir_blast, NULL},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++)
        httpd_register_uri_handler(server, &routes[i]);
    ESP_LOGI(TAG, "HTTP server on :80 (speaker)");
}
