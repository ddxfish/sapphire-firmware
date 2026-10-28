// server.c - see server.h. Plain HTTP on port 80, on the home network only.
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_jpeg_dec.h"

#include "audio.h"
#include "camera.h"
#include "capture.h"
#include "light.h"
#include "lastwords.h"
#include "link.h"
#include "mbedtls/base64.h"
#include "net.h"
#include "ota.h"
#include "settings.h"
#include "stats.h"
#include "storage.h"
#include "ui.h"
#include "wake.h"
#include "wav.h"
#include "board.h"
#include "buttons.h"

static const char *TAG = "server";
static httpd_handle_t server;

#define BOARD "waveshare-s3-audio"
#define SPEAK_CHUNK 4096
#define HALT_MS 3000                   // after her own stop (/audio/stop), the rest of that reply is refused this long: she has stopped sending too
#define HALT_HERE_MS 60000             // after a stop made on the board while her turn goes on: until it ends or a question is sent (server_reply_over), this at most
static volatile bool cut;              // /audio/stop while a sentence plays
static volatile bool speaking;
static _Atomic size_t queued_bytes;    // sound taken in, waiting in PSRAM for the player
static int late_last_ms, late_total_ms, late_count;     // a sentence that took longer to play than it is long (/health sound)
static int64_t halted_until_ms;
static QueueHandle_t speak_queue;      // /audio/speak runs on its own task: the server stays free to answer
static QueueHandle_t store_queue;      // /storage PUT, GET and format: a minute each, off the server task too
static QueueHandle_t glass_queue;      // /screen/picture and /screen/background: an image body, off the server task
static volatile bool ota_running;      // one program at a time, on its own internal-stack task
#define LISTEN_MAX_S 60
#define STORE_CHUNK 4096
static const uint8_t BAK_MAGIC[12] = "SAPPHIREBAK\x01";
#define VOICE_FIRST_MS 100             // while a question is recorded or sent, the card transfer waits
#define STORE_PACE_MS 8                // after each 4 KB of a backup: ~500 KB/s, so core 0 (WiFi + AFE) keeps idle time

// --- small answers ---------------------------------------------------------------

static esp_err_t say_json(httpd_req_t *req, int status, cJSON *json)
{
    httpd_resp_set_status(req, status == 200 ? "200 OK" : status == 400 ? "400 Bad Request"
                          : status == 401 ? "401 Unauthorized" : status == 404 ? "404 Not Found"
                          : status == 409 ? "409 Conflict" : status == 411 ? "411 Length Required"
                          : status == 413 ? "413 Payload Too Large" : status == 415 ? "415 Unsupported Media Type"
                          : status == 422 ? "422 Unprocessable Entity" : status == 503 ? "503 Service Unavailable"
                          : status == 507 ? "507 Insufficient Storage" : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    char *text = cJSON_PrintUnformatted(json);
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(json);
    return err;
}

static esp_err_t refuse(httpd_req_t *req, int status, const char *detail)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "detail", detail);
    return say_json(req, status, j);
}

static bool same(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = la ^ lb;
    for (size_t i = 0; i < la && i < lb; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

// The key, checked in constant time. 401 says nothing about which part was wrong.
static bool let_in(httpd_req_t *req)
{
    char auth[SETTING_MAX + 8] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof auth) != ESP_OK
        || strncmp(auth, "Bearer ", 7) || !settings.key[0] || !same(auth + 7, settings.key)) {
        refuse(req, 401, "wrong key");
        return false;
    }
    return true;
}

static bool query_value(httpd_req_t *req, const char *name, char *out, size_t size)
{
    char query[128];
    out[0] = 0;
    return httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK
        && httpd_query_key_value(query, name, out, size) == ESP_OK;
}

// --- /sensors: what the board measures, now (0.7.5). The battery, only when the schematic's
// jumper R78 lands the cell's divider on GPIO 6 (board.h); an open jumper reads a floating pin
// and the object is empty. A name ends in its unit, as the protocol asks -------------------

static cJSON *sensors_json(void)
{
    board_battery_t b;
    if (!board_battery_read(&b) || !b.battery) return NULL;
    cJSON *s = cJSON_CreateObject();
    cJSON_AddNumberToObject(s, "battery_v", (b.mv / 10) / 100.0);
    cJSON_AddNumberToObject(s, "battery_pct", b.percent);
    return s;
}

static esp_err_t on_sensors(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *j = cJSON_CreateObject();
    cJSON *s = sensors_json();
    cJSON_AddItemToObject(j, "sensors", s ? s : cJSON_CreateObject());
    return say_json(req, 200, j);
}

// --- /health -----------------------------------------------------------------------

static esp_err_t on_health(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "name", settings.name);
    cJSON_AddStringToObject(j, "board", BOARD);
    cJSON_AddStringToObject(j, "firmware", PROJECT_VER);
    cJSON_AddStringToObject(j, "mac", board_mac());             // the chip's own id: the flasher knows a board by it
    cJSON_AddStringToObject(j, "model", FIRMWARE_MODEL);         // which program this is: the firmware index's board id
    cJSON_AddStringToObject(j, "slot", ota_slot());
    cJSON *has = cJSON_AddArrayToObject(j, "has");
    if (audio_ok()) {                                           // a part that did not start is not claimed (0.5.2); `problems` says why
        cJSON_AddItemToArray(has, cJSON_CreateString("speaker"));
        cJSON_AddItemToArray(has, cJSON_CreateString("mic"));
        cJSON_AddItemToArray(has, cJSON_CreateString("wake"));
    }
    cJSON_AddItemToArray(has, cJSON_CreateString("light"));
    cJSON_AddItemToArray(has, cJSON_CreateString("power"));
    cJSON_AddItemToArray(has, cJSON_CreateString("buttons"));   // BOOT (0.6.0): what each press is set to comes on the events stream
    cJSON_AddItemToObject(j, "buttons", buttons_json());
    cJSON *sound = cJSON_AddObjectToObject(j, "sound");      // the play queue (0.7.7): late = played longer than it is long
    cJSON_AddNumberToObject(sound, "queued_kb", queued_bytes / 1024);
    cJSON_AddNumberToObject(sound, "late_last_ms", late_last_ms);
    cJSON_AddNumberToObject(sound, "late_count", late_count);
    cJSON_AddNumberToObject(sound, "late_total_ms", late_total_ms);
    const char *deaf;
    cJSON_AddNumberToObject(sound, "heals", board_speaker_heals(&deaf));   // a chip that reset under her voice, put back (board_speaker_check)
    cJSON_AddStringToObject(sound, "heal_last", deaf);
    if (ui_ok()) {
        cJSON_AddItemToArray(has, cJSON_CreateString("screen"));
        cJSON_AddItemToArray(has, cJSON_CreateString("picker"));   // the chat picker on the bottom place (0.9.1): the Devices page offers Browse chats
        cJSON *screen = cJSON_AddObjectToObject(j, "screen");    // what /screen/picture and /screen/background take
        cJSON_AddNumberToObject(screen, "w", BOARD_W);
        cJSON_AddNumberToObject(screen, "h", BOARD_H);
        cJSON_AddStringToObject(screen, "format", "rgb565be");
        cJSON_AddBoolToObject(screen, "jpeg", true);                      // /screen/picture?format=jpeg also takes one JPEG
        cJSON_AddNumberToObject(screen, "fps", UI_STREAM_FPS);           // frames sent one after another are painted on this beat (0.7.8)
        cJSON_AddBoolToObject(screen, "taps", true);                      // a stream may ask for the taps: ?taps=1 on its frames (0.9.0)
        const ui_zone_t *zones;                                           // the places a tap is named by, tiling the glass
        cJSON *places = cJSON_AddArrayToObject(screen, "zones");
        for (int n = ui_zones(&zones), i = 0; i < n; i++) {
            cJSON *z = cJSON_CreateObject();
            cJSON_AddStringToObject(z, "name", zones[i].name);
            cJSON_AddNumberToObject(z, "x", zones[i].x);
            cJSON_AddNumberToObject(z, "y", zones[i].y);
            cJSON_AddNumberToObject(z, "w", zones[i].w);
            cJSON_AddNumberToObject(z, "h", zones[i].h);
            cJSON_AddItemToArray(places, z);
        }
        int painted, replaced;
        ui_stream_counts(&painted, &replaced);
        cJSON *stream = cJSON_AddObjectToObject(screen, "stream");        // since boot: frames painted, frames dropped for a newer one
        cJSON_AddNumberToObject(stream, "painted", painted);
        cJSON_AddNumberToObject(stream, "replaced", replaced);
        cJSON_AddBoolToObject(screen, "on", ui_screen_on());
        cJSON_AddStringToObject(screen, "chat", ui_chat_name());          // the chat name it shows; core sends it with POST /screen {"chat": ...}
        cJSON_AddStringToObject(screen, "brain", ui_brain_name());
        char tint[8] = "";
        if (ui_trim() >= 0) snprintf(tint, sizeof tint, "#%06x", (unsigned)ui_trim());
        cJSON_AddStringToObject(screen, "trim", tint);
        cJSON_AddStringToObject(screen, "background", ui_scene_name());   // the scene it shows; core pushes when the chat's differs
        cJSON_AddBoolToObject(screen, "picture", ui_picture_on());
        cJSON_AddNumberToObject(screen, "brightness", settings.brightness);      // PUT /screen/settings
        cJSON_AddNumberToObject(screen, "dim", settings.dim_percent);
        cJSON_AddNumberToObject(screen, "dim_after_s", settings.dim_after_s);
        cJSON_AddNumberToObject(screen, "off_after_min", settings.off_after_min);
        cJSON_AddBoolToObject(screen, "flip", settings.flip);
    }
    cJSON *mute = cJSON_AddObjectToObject(j, "mute");
    cJSON_AddBoolToObject(mute, "speaker", audio_muted());
    if (camera_present()) {
        cJSON_AddItemToArray(has, cJSON_CreateString("camera"));
        cJSON *cam = cJSON_AddObjectToObject(j, "camera");
        cJSON_AddStringToObject(cam, "sensor", camera_sensor_name());
        cJSON_AddItemToObject(cam, "sizes", camera_sizes());    // what ?size= takes; 800x600 if left out
        cJSON_AddBoolToObject(cam, "on", camera_busy());         // powered only while a shot is taken
    }
    if (ota_can()) cJSON_AddItemToArray(has, cJSON_CreateString("firmware"));   // two slots: PUT /firmware (was inside the card's block until 0.3.2)
    cJSON *sensors = sensors_json();                             // the battery (0.7.5), only when its jumper lands it on the pin
    if (sensors) {
        cJSON_AddItemToArray(has, cJSON_CreateString("sensors"));
        cJSON_AddItemToObject(j, "sensors", sensors);            // name: number; Sapphire shows each as a reading
    }
    if (storage_ok()) {
        cJSON_AddItemToArray(has, cJSON_CreateString("storage"));
        cJSON *st = cJSON_AddObjectToObject(j, "storage");
        uint64_t free_b = 0, total = 0;
        if (storage_room(&free_b, &total)) {
            cJSON_AddNumberToObject(st, "free_bytes", (double)free_b);
            cJSON_AddNumberToObject(st, "total_bytes", (double)total);
        }
        cJSON *can = cJSON_AddArrayToObject(st, "can");
        cJSON_AddItemToArray(can, cJSON_CreateString("format"));
        storage_state_t card;
        storage_state(&card);
#ifdef BOARD_STORAGE_CHECKS
        cJSON_AddItemToArray(can, cJSON_CreateString("check"));
        cJSON *ch = cJSON_AddObjectToObject(st, "checks");
        cJSON_AddBoolToObject(ch, "running", card.checking);
        cJSON_AddNumberToObject(ch, "done", card.checked);
        cJSON_AddNumberToObject(ch, "of", card.to_check);
        cJSON_AddNumberToObject(ch, "damaged", card.damaged);
        cJSON_AddNumberToObject(ch, "unchecked", card.unchecked);
#endif
        cJSON_AddNumberToObject(st, "count", card.count);
        cJSON_AddBoolToObject(st, "writing", card.writing);
        cJSON_AddBoolToObject(st, "failed", card.failed);
        cJSON_AddStringToObject(st, "last_error", storage_last_error());   // what the card said the last time a write went wrong; "" after a good one
    }
    cJSON *plays = cJSON_AddObjectToObject(j, "plays");
    cJSON_AddStringToObject(plays, "type", "audio/wav");
    cJSON_AddNumberToObject(plays, "rate", AUDIO_RATE);
    cJSON_AddNumberToObject(plays, "channels", 1);
    cJSON_AddNumberToObject(j, "volume", audio_volume());
    cJSON_AddNumberToObject(j, "volume_step", settings.vol_step);   // what a volume button adds or takes: POST /volume?step=
    cJSON_AddItemToObject(j, "led", light_status());
    cJSON_AddNumberToObject(j, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON *ww = cJSON_AddObjectToObject(j, "wakeword");
    cJSON_AddBoolToObject(ww, "enabled", wake_enabled());
    cJSON_AddBoolToObject(ww, "running", wake_running());
    cJSON_AddBoolToObject(ww, "paused", wake_paused());         // a backup is landing: it hears nothing until that ends
    cJSON_AddStringToObject(ww, "model", wake_model_name());
    cJSON_AddStringToObject(ww, "format", "tflite");            // microWakeWord: the Wakeword Maker sends this family
    cJSON_AddItemToObject(j, "capture", capture_json());         // the room in pieces (0.7.6): a board with this key has the door
    cJSON *link = cJSON_AddObjectToObject(j, "link");
    cJSON_AddBoolToObject(link, "connected", link_connected());
    cJSON_AddNumberToObject(j, "wifi_dbm", net_rssi());
    cJSON_AddNumberToObject(j, "free_kb", esp_get_free_heap_size() / 1024);
    cJSON_AddNumberToObject(j, "free_internal_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(j, "free_internal_min_kb", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);   // the least it has been since boot
    cJSON_AddNumberToObject(j, "free_internal_largest_kb", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024);
    if (stats_idle(0) >= 0) {                                   // the idle share of each core over the last 2 s
        cJSON *cpu = cJSON_AddObjectToObject(j, "cpu");
        cJSON_AddNumberToObject(cpu, "idle0", (int)(stats_idle(0) + 0.5f));
        cJSON_AddNumberToObject(cpu, "idle1", (int)(stats_idle(1) + 0.5f));
    }
    cJSON *mic = cJSON_AddObjectToObject(j, "mic");
    cJSON_AddNumberToObject(mic, "gain_db", audio_mic_gain());
    cJSON_AddBoolToObject(mic, "agc", settings.agc);
    cJSON_AddNumberToObject(mic, "max_s", settings.question_max_s);
    // how it is doing (0.5.2): what did not start, how this boot began, why the last one ended
    cJSON *problems = cJSON_AddArrayToObject(j, "problems");
    if (board_trouble()[0]) cJSON_AddItemToArray(problems, cJSON_CreateString(board_trouble()));
    else if (!audio_ok()) cJSON_AddItemToArray(problems, cJSON_CreateString("the sound pipeline did not start"));
    if (lastwords_strikes() >= 3) cJSON_AddItemToArray(problems, cJSON_CreateString("the glass is off: it stopped three starts in a row (a restart tries again)"));
    else if (!board_trouble()[0] && !ui_ok()) cJSON_AddItemToArray(problems, cJSON_CreateString("no glass answered"));
    cJSON_AddStringToObject(j, "reset", lastwords_reset());
    cJSON *words = lastwords_json(false);
    if (words) cJSON_AddItemToObject(j, "last_words", words);
    int errors, resets, held;
    board_i2c_counts(&errors, &resets, &held);
    cJSON *bus = cJSON_AddObjectToObject(j, "bus");
    cJSON_AddNumberToObject(bus, "touch_errors", errors);
    cJSON_AddNumberToObject(bus, "touch_rests", resets);
    cJSON_AddNumberToObject(bus, "held_at_boot", held);
    cJSON_AddNumberToObject(bus, "glass_refused", ui_doors_shut());
    return say_json(req, 200, j);
}

// --- /audio/speak: play the body as it arrives ---------------------------------------
// The server has one task. A sentence plays for seconds, so the handler hands
// the request to the speak task (httpd's async handlers) and the server goes
// on answering: /audio/stop, /health, the light. Sentences queue in order.

// A sentence is taken whole into PSRAM at WiFi speed, then played from there by
// the player (0.7.7): a WiFi stall (battery, distance, a busy radio) can only
// delay a sentence's start, never punch a hole in it, and the next sentence is
// already waiting when one ends. Before, it played straight off the socket with
// ~0.4 s of slack. A stop bumps `stop_gen`: what was queued before it is dropped.
#define SPEAK_QUEUED_MAX (1536 * 1024)  // sound waiting to play, all of it: ~47 s at 16 kHz mono
typedef struct { uint8_t *buf; size_t len, at, samples; int channels; unsigned gen; } sentence_t;
#define SPEAK_ROOM_SLACK_S 10           // waiting for room: as long as what is queued plays, and this
static QueueHandle_t play_queue;
static _Atomic unsigned stop_gen;
static volatile bool receiving;         // a sentence is being taken in: she is talking (server_talking)

// Room for `len` more in the play queue: wait while what is queued plays, so a
// reply made faster than she speaks is held back at the door (its sender waits
// on this answer) and not refused. Before (0.7.7 first cut) a full queue was a
// 503, which Sapphire takes for "stopped", and a long answer lost its end.
// False on a stop meanwhile, or when it never makes room.
static bool wait_for_room(size_t len, unsigned gen)
{
    int64_t until = esp_timer_get_time() + ((int64_t)queued_bytes / (AUDIO_RATE * 2) + SPEAK_ROOM_SLACK_S) * 1000000;
    while (queued_bytes + len > SPEAK_QUEUED_MAX || !uxQueueSpacesAvailable(play_queue)) {
        if (gen != stop_gen || esp_timer_get_time() > until) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

static void speak_work(httpd_req_t *req)
{
    size_t len = req->content_len;
    bool stopped = esp_timer_get_time() / 1000 < halted_until_ms || wake_busy();   // the rest of a stopped reply, or a sentence while the board hears a question: taken, not played
    unsigned gen = stop_gen;
    uint8_t *buf = NULL;
    int problem = 0;
    if (!stopped) {
        receiving = true;
        if (!len || len > SPEAK_QUEUED_MAX) problem = 413;
        else if (!wait_for_room(len, gen)) { if (gen != stop_gen) stopped = true; else problem = 503; }
        else if (!(buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM))) problem = 507;
    }
    uint8_t drain[512];
    for (size_t have = 0; have < len && !problem;) {
        int n = httpd_req_recv(req, buf ? (char *)buf + have : (char *)drain,
                               buf ? (len - have > SPEAK_CHUNK ? SPEAK_CHUNK : len - have) : sizeof drain);
        if (n <= 0) problem = 400;
        else have += n;
    }
    wav_info_t info = {0};
    if (buf && !problem && (!wav_parse(buf, len, &info) || info.rate != AUDIO_RATE)) problem = 415;
    if (buf && !problem) {
        size_t data = wav_get32(buf + info.data_at - 4);              // the data chunk's own size: a chunk after it is not sound
        if (data > len - info.data_at) data = len - info.data_at;
        sentence_t s = {.buf = buf, .len = len, .at = info.data_at, .channels = info.channels, .gen = gen,
                        .samples = (data / 2) & ~(size_t)(info.channels - 1)};
        queued_bytes += len;
        if (xQueueSend(play_queue, &s, pdMS_TO_TICKS(1000)) == pdTRUE) buf = NULL;
        else { queued_bytes -= len; problem = 503; }
    }
    free(buf);
    receiving = false;
    if (!problem && gen != stop_gen) stopped = true;      // a stop landed while it came in: the player drops it
    if (problem == 415) refuse(req, 415, "not a 16 kHz 16 bit PCM WAV");
    else if (problem == 413) refuse(req, 413, "one sentence of up to 47 s");
    else if (problem == 503) refuse(req, 503, "too much sound is waiting to play");
    else if (problem == 507) refuse(req, 507, "no memory for the sound");
    else if (problem) refuse(req, 400, "the sound did not arrive whole");
    else {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        if (stopped) cJSON_AddBoolToObject(j, "stopped", true);
        say_json(req, 200, j);
    }
    httpd_req_async_handler_complete(req);
}

static void play_task(void *arg)
{
    sentence_t s;
    for (;;) {
        if (xQueueReceive(play_queue, &s, portMAX_DELAY) != pdTRUE) continue;
        bool play = s.gen == stop_gen;
        if (play && !audio_take_speaker(pdMS_TO_TICKS(2000))) {
            play = false;
            ESP_LOGW(TAG, "the speaker stayed busy 2 s: a sentence of %u ms was dropped", (unsigned)(s.samples / s.channels * 1000 / AUDIO_RATE));
        }
        if (play) {
            cut = false;
            if (s.gen != stop_gen) { audio_give_speaker(); play = false; }   // a stop landed while it waited for the speaker
        }
        if (play) {
            speaking = true;
            light_cue("speaking");
            int64_t t0 = esp_timer_get_time();
            const int16_t *pcm = (const int16_t *)(s.buf + s.at);
            size_t step = (SPEAK_CHUNK / 2) & ~(size_t)(s.channels - 1);
            for (size_t done = 0; done < s.samples && !cut; done += step) {
                size_t n = s.samples - done < step ? s.samples - done : step;
                audio_play(pcm + done, n, s.channels);
            }
            audio_play_end();
            if (!cut) {
                int late = (int)((esp_timer_get_time() - t0) / 1000) - (int)(s.samples / s.channels * 1000 / AUDIO_RATE) - 50;
                late_last_ms = late > 0 ? late : 0;
                if (late > 40) { late_total_ms += late; late_count++; ESP_LOGW(TAG, "a sentence played %d ms late", late); }
            }
            speaking = false;
            audio_give_speaker();
        }
        queued_bytes -= s.len;
        free(s.buf);
        if (!uxQueueMessagesWaiting(play_queue)) light_speech_done();   // the brain's cue while its turn goes on, else idle
    }
}

static void speak_task(void *arg)
{
    httpd_req_t *req;
    for (;;) {
        if (xQueueReceive(speak_queue, &req, portMAX_DELAY) == pdTRUE) speak_work(req);
    }
}

static esp_err_t on_speak(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    char kind[48] = {0};
    httpd_req_get_hdr_value_str(req, "Content-Type", kind, sizeof kind);
    if (strncmp(kind, "audio/wav", 9) && strncmp(kind, "audio/x-wav", 11))
        return refuse(req, 415, "this board plays audio/wav, 16000 Hz, one channel, 16 bit");
    if (!audio_ok())
        return refuse(req, 500, "the sound is not running on this board");
    if (!uxQueueSpacesAvailable(speak_queue))
        return refuse(req, 409, "already playing something, with one more waiting");
    httpd_req_t *held;
    if (httpd_req_async_handler_begin(req, &held) != ESP_OK)
        return refuse(req, 500, "could not take the sound");
    xQueueSend(speak_queue, &held, 0);
    return ESP_OK;
}

// --- /audio/stop: cut the sentence playing, refuse the rest of that reply for a moment -----

bool server_speaking(void) { return speaking; }

// Sound is playing or waiting to. Between two sentences of a reply it is
// light_turn_on() that says she is still at it; a timed tail here swallowed
// the tap that answers her and refused a minute of speech (scout, 2026-10-11).
bool server_talking(void) { return speaking || receiving || (speak_queue && uxQueueMessagesWaiting(speak_queue)) || (play_queue && uxQueueMessagesWaiting(play_queue)); }

static bool stop_for(int ms)
{
    bool was = speaking || (play_queue && uxQueueMessagesWaiting(play_queue));
    stop_gen++;                                          // and every sentence queued before this stop: first,
    cut = true;                                          // so the player cannot clear `cut` for a sentence this stop dooms
    halted_until_ms = esp_timer_get_time() / 1000 + ms;
    return was;
}

// A stop made on the board (the middle of the glass, a button, the wake word
// said over her): the sentence is cut and every one after it is answered
// "stopped", which is how Sapphire learns to drop the rest of the reply.
// The long hold only while her turn goes on: its end clears it. A plain
// announcement has no turn to end it, so it gets the short one.
bool server_stop_speech(void) { return stop_for(light_turn_on() ? HALT_HERE_MS : HALT_MS); }

// The same by a hand (the middle of the glass, a button): Sapphire is told too,
// so a turn that is still thinking ends now and not at its next sentence. Not
// for the wake word said over her: claiming the wake tells her already, and a
// second word could land on the new question's turn.
bool server_stop_by_hand(void)
{
    bool turn = light_turn_on(), was = server_stop_speech();
    if (turn || was) link_ask("stop");                  // an announcement too (0.7.7): it was answered when taken, so its "stopped" is told here
    return was;
}

// The stopped reply is over: her turn ended, or a new question went to her.
void server_reply_over(void) { halted_until_ms = 0; }

static esp_err_t on_stop(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    bool was = stop_for(HALT_MS);
    ESP_LOGI(TAG, "audio/stop: %s", was ? "cut the playback" : "nothing was playing");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "stopped", was);
    return say_json(req, 200, j);
}

// --- /audio/listen: record until the speaker stops, or (vad=false) the room for `seconds` -----
// On its own task (0.7.7): a speak is answered once taken, so a `say` then a
// listen arrived while she was still saying it, and the board recorded her
// own voice. It waits here until she is done (bounded: Sapphire gives a listen
// its seconds + 12), with the server free for a stop meanwhile.
#define LISTEN_WAIT_S 10
typedef struct { httpd_req_t *req; int max_s; bool vad; } listen_job_t;
static QueueHandle_t listen_queue;

static void listen_work(listen_job_t *job)
{
    httpd_req_t *req = job->req;
    int64_t until = esp_timer_get_time() + LISTEN_WAIT_S * 1000000LL;
    bool waited = false;
    while (server_talking() && esp_timer_get_time() < until) { waited = true; vTaskDelay(pdMS_TO_TICKS(50)); }
    if (waited) vTaskDelay(pdMS_TO_TICKS(150));          // her last words leave the DMA and the room
    recording_t rec;
    if (!wake_record_now(job->max_s, job->vad, &rec)) {
        refuse(req, 409, "already listening for a question, or no sound is coming in from the microphones");
    } else {
        uint8_t head[WAV_HEADER];
        wav_header(head, AUDIO_RATE, 1, rec.samples * 2);
        httpd_resp_set_type(req, "audio/wav");
        httpd_resp_send_chunk(req, (char *)head, WAV_HEADER);
        for (size_t at = 0; at < rec.samples; at += SPEAK_CHUNK / 2) {
            size_t n = rec.samples - at < SPEAK_CHUNK / 2 ? rec.samples - at : SPEAK_CHUNK / 2;
            httpd_resp_send_chunk(req, (char *)(rec.pcm + at), n * 2);
        }
        httpd_resp_send_chunk(req, NULL, 0);
        wake_record_done(&rec);
    }
    httpd_req_async_handler_complete(req);
}

static void listen_task(void *arg)
{
    listen_job_t job;
    for (;;)
        if (xQueueReceive(listen_queue, &job, portMAX_DELAY) == pdTRUE) listen_work(&job);
}

static esp_err_t on_listen(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    char text[16];
    int max_s = query_value(req, "max_seconds", text, sizeof text) ? atoi(text) : 15;
    if (max_s < 1) max_s = 1;
    if (max_s > LISTEN_MAX_S) max_s = LISTEN_MAX_S;
    bool use_vad = !(query_value(req, "vad", text, sizeof text) && (text[0] == 'f' || text[0] == '0'));
    if (!use_vad && query_value(req, "seconds", text, sizeof text)) {       // the room: `seconds` names the length
        max_s = atoi(text);
        if (max_s < 1) max_s = 1;
        if (max_s > LISTEN_MAX_S) max_s = LISTEN_MAX_S;
    }
    if (!uxQueueSpacesAvailable(listen_queue))
        return refuse(req, 409, "already listening for a question, or no sound is coming in from the microphones");
    listen_job_t job = {.max_s = max_s, .vad = use_vad};
    if (httpd_req_async_handler_begin(req, &job.req) != ESP_OK) return refuse(req, 500, "could not take the request");
    xQueueSend(listen_queue, &job, 0);
    return ESP_OK;
}

// --- /led: the ring as her light -----------------------------------------------------------

#define JSON_MAX 4096

// The request body as JSON, or NULL (and a 400 already sent).
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len > JSON_MAX) { refuse(req, 413, "too much"); return NULL; }
    char *body = malloc(req->content_len + 1);
    size_t have = 0;
    while (have < req->content_len) {
        int n = httpd_req_recv(req, body + have, req->content_len - have);
        if (n <= 0) { free(body); refuse(req, 400, "the body did not arrive whole"); return NULL; }
        have += n;
    }
    body[have] = 0;
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) refuse(req, 400, "the body is not JSON");
    return j;
}

static esp_err_t on_led(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *in = read_json(req);
    if (!in) return ESP_OK;
    const cJSON *state = cJSON_GetObjectItem(in, "state");
    cJSON *out = cJSON_CreateObject();
    if (cJSON_IsString(state) && !strcmp(state->valuestring, "off")) {
        light_dark();
        cJSON_AddStringToObject(out, "state", "off");
    } else if (cJSON_IsString(state) && !strcmp(state->valuestring, "idle")) {
        light_release();
        cJSON_AddStringToObject(out, "state", "idle");
    } else {
        look_t look;
        if (!light_parse(in, &look, &LIGHT_PLAIN)) {
            cJSON_Delete(in); cJSON_Delete(out);
            return refuse(req, 400, "a color or animation this ring does not know; GET /led/spec lists them");
        }
        const cJSON *d = cJSON_GetObjectItem(in, "duration_s");
        float seconds = cJSON_IsNumber(d) ? (float)d->valuedouble : 300;
        light_hold(&look, seconds);
        cJSON_AddStringToObject(out, "state", "custom");
        cJSON *led = light_json(&look);
        cJSON_AddNumberToObject(led, "duration_s", seconds);
        cJSON_AddItemToObject(out, "led", led);
    }
    cJSON_Delete(in);
    return say_json(req, 200, out);
}

static esp_err_t on_led_spec(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    return say_json(req, 200, light_spec());
}

static esp_err_t on_led_baseline(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (req->method == HTTP_PUT) {
        cJSON *in = read_json(req);
        if (!in) return ESP_OK;
        look_t look;
        bool ok = light_parse(in, &look, light_look(LOOK_RESTING));
        cJSON_Delete(in);
        if (!ok) return refuse(req, 400, "a color or animation this ring does not know; GET /led/spec lists them");
        light_set_look(LOOK_RESTING, &look);
        light_save();
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddItemToObject(out, "baseline", light_json(light_look(LOOK_RESTING)));
    return say_json(req, 200, out);
}

static int minutes(const cJSON *j, const char *name, int fallback)
{
    const cJSON *v = cJSON_GetObjectItem(j, name);
    int h, m;
    if (!cJSON_IsString(v) || sscanf(v->valuestring, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return fallback;
    return (h * 60 + m) % (24 * 60);
}

static esp_err_t on_led_looks(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *in = read_json(req);
    if (!in) return ESP_OK;
    for (int i = 0; i < LOOK_COUNT; i++) {
        const cJSON *one = cJSON_GetObjectItem(in, LIGHT_SLOTS[i]);
        look_t look;
        if (!one) continue;
        if (!light_parse(one, &look, light_look(i))) {
            cJSON_Delete(in);
            char why[80];
            snprintf(why, sizeof why, "the %s look names a color or animation this ring does not know", LIGHT_SLOTS[i]);
            return refuse(req, 400, why);
        }
        light_set_look(i, &look);
    }
    light_set_hours(minutes(in, "from", 0), minutes(in, "until", 0));
    cJSON_Delete(in);
    light_save();
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddItemToObject(out, "spec", light_spec());
    return say_json(req, 200, out);
}

// --- /capture: the room in pieces to Sapphire (0.7.6) ---------------------------------------
// POST ?seconds=36000&chunk=30&test=false begins; the board posts every piece to her room door from its own
// task and keeps hearing its wake word. DELETE ends it. GET says how it goes; /health carries the same.

static esp_err_t on_capture(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (req->method == HTTP_DELETE) {
        capture_stop();
    } else if (req->method == HTTP_POST) {
        char text[16];
        int seconds = query_value(req, "seconds", text, sizeof text) ? atoi(text) : 3600;
        int chunk = query_value(req, "chunk", text, sizeof text) ? atoi(text) : 30;
        bool test = query_value(req, "test", text, sizeof text) && (text[0] == 't' || text[0] == '1');
        char why[96];
        if (!capture_start(seconds, chunk, test, why, sizeof why)) return refuse(req, 409, why);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddItemToObject(j, "capture", capture_json());
    return say_json(req, 200, j);
}

// --- /wakeword, /power, /sounds ------------------------------------------------------------

static esp_err_t on_wakeword(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (req->method == HTTP_POST) {
        char text[8];
        if (!query_value(req, "enabled", text, sizeof text)) return refuse(req, 400, "enabled=true or false");
        wake_enable(!strcmp(text, "true") || !strcmp(text, "1"));
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", wake_enabled());
    cJSON_AddBoolToObject(j, "running", wake_running());
    cJSON_AddStringToObject(j, "model", wake_model_name());
    cJSON_AddStringToObject(j, "format", "tflite");
    cJSON_AddNumberToObject(j, "threshold", wake_model_threshold());
    cJSON_AddBoolToObject(j, "stored", wake_model_stored());
    return say_json(req, 200, j);
}

// '+' and %xx in a query value
static void unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++, w++) {
        if (*r == '+') *w = ' ';
        else if (*r == '%' && r[1] && r[2]) { unsigned v = 0; sscanf(r + 1, "%2x", &v); *w = (char)v; r += 2; }
        else *w = *r;
    }
    *w = 0;
}

// PUT /wakeword/model?name=hey_marcus&threshold=0.97&format=tflite&sliding_window=5&step_ms=10&phrase=hey+marcus
// with the .tflite as the body. Stored, then the board restarts on it. DELETE goes back to the embedded model.
static esp_err_t on_wakeword_model(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (ota_running || storage_busy()) return refuse(req, 409, "busy: a program or a backup is arriving, and this restarts the board");
    settings_flush();
    if (req->method == HTTP_DELETE) {
        wake_model_forget();
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "model", "hey_jarvis");
        cJSON_AddNumberToObject(j, "restart_in_s", 2);
        esp_err_t r = say_json(req, 200, j);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        return r;
    }
    char query[384] = {0}, name[48] = {0}, phrase[64] = {0}, text[24] = {0};
    httpd_req_get_url_query_str(req, query, sizeof query);
    if (httpd_query_key_value(query, "format", text, sizeof text) == ESP_OK && strcmp(text, "tflite"))
        return refuse(req, 415, "this board takes a microWakeWord .tflite (format=tflite)");
    if (httpd_query_key_value(query, "name", name, sizeof name) != ESP_OK || !name[0]) return refuse(req, 400, "name=<model name> is needed");
    unescape(name);
    for (char *c = name; *c; c++) if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') *c = '_';
    httpd_query_key_value(query, "phrase", phrase, sizeof phrase); unescape(phrase);
    float threshold = 0.97f;
    if (httpd_query_key_value(query, "threshold", text, sizeof text) == ESP_OK) threshold = atof(text);
    if (threshold < 0.05f || threshold > 0.999f) threshold = 0.97f;
    int window = 5, step = 10;
    if (httpd_query_key_value(query, "sliding_window", text, sizeof text) == ESP_OK) window = atoi(text);
    if (httpd_query_key_value(query, "step_ms", text, sizeof text) == ESP_OK) step = atoi(text);
    if (window < 1 || window > 50) return refuse(req, 400, "sliding_window is 1 to 50");
    if (step < 5 || step > 30) return refuse(req, 400, "step_ms is 5 to 30: the model's window is 30 ms");
    size_t size = req->content_len;
    if (size < 4096 || size > 1536 * 1024) return refuse(req, 413, "a model is 4 KB to 1.5 MB");
    uint8_t *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!data) return refuse(req, 500, "no memory for the model");
    size_t have = 0;
    while (have < size) {
        int got = httpd_req_recv(req, (char *)data + have, size - have);
        if (got <= 0) { free(data); return refuse(req, 400, "the model did not arrive whole"); }
        have += got;
    }
    char why[96] = {0};
    bool ok = wake_model_store(data, size, name, phrase, threshold, window, step, why, sizeof why);
    free(data);
    if (!ok) return refuse(req, 400, why);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "model", name);
    cJSON_AddNumberToObject(j, "threshold", threshold);
    cJSON_AddNumberToObject(j, "bytes", size);
    cJSON_AddNumberToObject(j, "restart_in_s", 2);
    cJSON_AddStringToObject(j, "note", "stored; restarting to listen for it");
    esp_err_t r = say_json(req, 200, j);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return r;
}

static void power_task(void *what)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (what) {
        ESP_LOGI(TAG, "shutdown: sleeping until the boot button, or a replug");
        audio_stop();
        esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
        esp_deep_sleep_start();
    }
    esp_restart();
}

static esp_err_t on_power(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    char text[16];
    if (!query_value(req, "action", text, sizeof text)) return refuse(req, 400, "action=restart or shutdown");
    bool off = !strcmp(text, "shutdown");
    if (!off && strcmp(text, "restart")) return refuse(req, 400, "action=restart or shutdown");
    settings_flush();                                   // what a slider or a key changed a moment ago
    xTaskCreate(power_task, "power", 3072, off ? (void *)1 : NULL, 5, NULL);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "action", text);
    cJSON_AddNumberToObject(j, "in_s", 2);
    return say_json(req, 200, j);
}

// A number and nothing else. "abc" was taken for 0: a mic at 0 dB hears no
// wake word, and it was saved (scout, 2026-10-11).
static bool number_of(const char *text, double *out)
{
    char *end;
    double v = strtod(text, &end);
    if (end == text || *end || v != v) return false;
    *out = v;
    return true;
}

static esp_err_t on_volume(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (req->method == HTTP_POST) {
        char text[8];
        bool any = false;
        double v;
        if (query_value(req, "step", text, sizeof text)) {       // what one press of a volume button adds or takes
            if (!number_of(text, &v)) return refuse(req, 400, "step is a number, 1 to 50");
            int n = (int)v;
            n = n < 1 ? 1 : n > 50 ? 50 : n;
            if (n != settings.vol_step) { settings.vol_step = n; settings_save(); }
            any = true;
        }
        if (query_value(req, "level", text, sizeof text)) {
            if (!number_of(text, &v)) return refuse(req, 400, "level is a number, 0 to 100");
            audio_set_volume((int)v);
            any = true;
        }
        if (!any) return refuse(req, 400, "level=0 to 100, and/or step=1 to 50");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "volume", audio_volume());
    cJSON_AddNumberToObject(j, "step", settings.vol_step);
    return say_json(req, 200, j);
}

// --- /mic: the microphones' gain and the AFE's automatic gain control ------------------------------
//   GET  /mic                      {"gain_db": 30, "gain_max_db": 37.5, "agc": false}
//   POST /mic?gain=36              live and saved
//   POST /mic?agc=on|off           saved; the board restarts in two seconds, the AFE only reads it at start
//   POST /mic?max_s=30             saved: a question is cut off at this many seconds (5 to 60), from the next one on

static esp_err_t on_mic(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    bool restart = false;
    if (req->method == HTTP_POST) {
        char text[8];
        bool any = false;
        double v;
        if (query_value(req, "gain", text, sizeof text)) {
            if (!number_of(text, &v)) return refuse(req, 400, "gain is a number of dB, 0 to 37.5");
            audio_set_mic_gain((float)v);
            any = true;
        }
        if (query_value(req, "agc", text, sizeof text)) {
            bool want = text[0] == 'o' ? text[1] == 'n' : (text[0] == '1' || text[0] == 't');
            if (want != settings.agc) {
                if (ota_running || storage_busy()) return refuse(req, 409, "busy: a program or a backup is arriving, and this restarts the board");
                settings.agc = want; settings_save(); restart = true;
            }
            any = true;
        }
        if (query_value(req, "max_s", text, sizeof text)) {
            if (!number_of(text, &v)) return refuse(req, 400, "max_s is a number of seconds, 5 to 60");
            int n = (int)v;
            n = n < QUESTION_MAX_S_LEAST ? QUESTION_MAX_S_LEAST : n > QUESTION_MAX_S_MOST ? QUESTION_MAX_S_MOST : n;
            if (n != settings.question_max_s) { settings.question_max_s = n; settings_save(); }
            any = true;
        }
        if (!any) return refuse(req, 400, "gain=0..37.5, agc=on|off and/or max_s=5..60");
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "gain_db", audio_mic_gain());
    cJSON_AddNumberToObject(j, "gain_max_db", MIC_GAIN_X2_MAX / 2.0);
    cJSON_AddBoolToObject(j, "agc", settings.agc);
    cJSON_AddNumberToObject(j, "max_s", settings.question_max_s);
    if (restart) cJSON_AddStringToObject(j, "note", "restarting in 2 s: the AFE reads agc at start");
    esp_err_t r = say_json(req, 200, j);
    if (restart) { vTaskDelay(pdMS_TO_TICKS(2000)); esp_restart(); }
    return r;
}

static esp_err_t on_sounds(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddArrayToObject(j, "sounds");
    return say_json(req, 200, j);
}

// POST /storage/check: every backup read back against the hash kept when it was written, now.
// It runs by itself; /health `storage.checks` says how far it is and what it found.
static esp_err_t on_storage_check(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!storage_ok()) return refuse(req, 503, "no card");
#ifdef BOARD_STORAGE_CHECKS
    if (!storage_check_now()) return refuse(req, 409, "a check is running already");
#endif
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "checking", true);
    return say_json(req, 200, j);
}

static esp_err_t on_missing(httpd_req_t *req, httpd_err_code_t err)
{
    return refuse(req, 404, "this board has no such address");
}

// --- /storage: backups on the card (storage.h) -------------------------------------------
// Sapphire PUTs a sealed backup, lists, GETs one back for a restore, DELETEs
// the oldest, and the person at the Devices page may format. The long ones
// (PUT, GET, format) run on the store task, like /audio/speak, so the server
// keeps answering /health and her voice while a backup lands.

static const char *file_name(httpd_req_t *req)
{
    const char *p = req->uri + strlen("/storage/");
    return p;
}

static esp_err_t on_storage(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!storage_alive()) return refuse(req, 503, "no card");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "path", storage_dir());
    uint64_t free_b = 0, total = 0;
    if (storage_room(&free_b, &total)) {
        cJSON_AddNumberToObject(j, "free_bytes", (double)free_b);
        cJSON_AddNumberToObject(j, "total_bytes", (double)total);
    }
    cJSON *can = cJSON_AddArrayToObject(j, "can");
    cJSON_AddItemToArray(can, cJSON_CreateString("format"));
    cJSON *files = cJSON_AddArrayToObject(j, "files");
    DIR *d = opendir(storage_dir());
    if (d) {
        struct dirent *e;
        char path[200];
        struct stat st;
        while ((e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (n > 8 && !strcmp(e->d_name + n - 8, ".partial")) continue;
            if (n > 7 && !strcmp(e->d_name + n - 7, ".sha256")) continue;      // the board's own, beside each backup
            if (!storage_path(e->d_name, path, sizeof path) || stat(path, &st) || !S_ISREG(st.st_mode)) continue;
            cJSON *f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "name", e->d_name);
            cJSON_AddNumberToObject(f, "size", (double)st.st_size);
            cJSON_AddNumberToObject(f, "mtime", (double)st.st_mtime);
#ifdef BOARD_STORAGE_CHECKS
            int checked = storage_checked(e->d_name);          // "ok": false = damaged; absent = no hash was kept
            if (checked >= 0 && n > 12 && !strcmp(e->d_name + n - 12, ".sapphirebak")) cJSON_AddBoolToObject(f, "ok", checked == 1);
#endif
            cJSON_AddItemToArray(files, f);
        }
        closedir(d);
    }
    return say_json(req, 200, j);
}

static void store_put(httpd_req_t *req)
{
    const char *name = file_name(req);
    bool plain_ok = storage_name_plain_ok(name);
    storage_write_t *w = storage_write_begin(name, req->content_len);
    if (!w) { refuse(req, 409, "another backup is arriving, or the card is gone"); goto done; }
    // A backup is the board's whole job while it lands and is read back (0.7.0):
    // no wake word, no question, the ring and the glass say so. Her question
    // during one reached nobody (2026-10-10): the stream holds the WiFi buffers.
    bool quiet = !plain_ok;
    if (quiet) { wake_pause(true); light_backup(true); ui_status("backup"); }
    uint8_t *buf = heap_caps_malloc(STORE_CHUNK, MALLOC_CAP_SPIRAM);
    size_t left = req->content_len, seen = 0;
    uint8_t head[12];
    int problem = 0;
    while (left > 0 && !problem) {
        // Her question comes first. A 70 MB stream fills the WiFi receive
        // buffers (internal RAM), and the voice POST needs that RAM for its
        // TLS session: the red ring of 2026-10-06. Not reading lets the TCP
        // window fill, Sapphire's send blocks (STORE_WAIT), nothing is lost.
        while (wake_busy()) vTaskDelay(pdMS_TO_TICKS(VOICE_FIRST_MS));
        int n = httpd_req_recv(req, (char *)buf, left < STORE_CHUNK ? left : STORE_CHUNK);
        if (n <= 0) { problem = 400; break; }
        left -= n;
        if (seen < sizeof head) {                          // the magic, before anything else lands
            size_t take = sizeof head - seen < (size_t)n ? sizeof head - seen : (size_t)n;
            memcpy(head + seen, buf, take);
            seen += take;
            if (!plain_ok && seen == sizeof head && memcmp(head, BAK_MAGIC, sizeof head)) { problem = 422; break; }
        }
        if (!storage_write_feed(w, buf, n)) { problem = 507; break; }
        vTaskDelay(pdMS_TO_TICKS(STORE_PACE_MS));
    }
    free(buf);
    if (!problem && !plain_ok && seen < sizeof head) problem = 422;
    char why[160];                                             // a card's own words (errno) travel to Sapphire's log (0.7.8)
    if (problem) {
        storage_write_drop(w);
        if (problem == 422) refuse(req, 400, "not a sealed backup: plaintext never lands on this card");
        else if (problem == 507) {
            snprintf(why, sizeof why, "the card failed while writing (%s)", storage_last_error());
            refuse(req, 507, why);
        } else refuse(req, 400, "the backup did not arrive whole");
        goto awake;
    }
    char sha[65];
    uint64_t size = 0;
    if (!storage_write_finish(w, sha, &size)) {
        snprintf(why, sizeof why, "the card could not keep the backup (%s)", storage_last_error());
        refuse(req, 507, why);
        goto awake;
    }
    char want[72] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Sha256", want, sizeof want) == ESP_OK && want[0] && strcasecmp(want, sha)) {
        storage_remove(name);                                  // and the hash beside it
        refuse(req, 422, "sha256 mismatch: dropped");
        goto awake;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "name", name);
    cJSON_AddNumberToObject(j, "size", (double)size);
    cJSON_AddStringToObject(j, "sha256", sha);
    say_json(req, 200, j);
awake:
    if (quiet) { wake_pause(false); light_backup(false); ui_status(""); }
done:
    httpd_req_async_handler_complete(req);
}

static void store_get(httpd_req_t *req)
{
    char path[200];
    FILE *f = storage_path(file_name(req), path, sizeof path) ? fopen(path, "rb") : NULL;
    if (!f) { refuse(req, 404, "no such file"); httpd_req_async_handler_complete(req); return; }
    httpd_resp_set_type(req, "application/octet-stream");
    uint8_t *buf = heap_caps_malloc(STORE_CHUNK, MALLOC_CAP_SPIRAM);
    size_t n;
    bool ok = true;
    while (ok && (n = fread(buf, 1, STORE_CHUNK, f)) > 0) {
        while (wake_busy()) vTaskDelay(pdMS_TO_TICKS(VOICE_FIRST_MS));
        ok = httpd_resp_send_chunk(req, (char *)buf, n) == ESP_OK;
    }
    if (ok) httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    fclose(f);
    httpd_req_async_handler_complete(req);
}

static void store_format(httpd_req_t *req)
{
    if (storage_format()) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        say_json(req, 200, j);
    } else refuse(req, 409, "could not format: busy, or the card is gone");
    httpd_req_async_handler_complete(req);
}

typedef struct { httpd_req_t *req; void (*work)(httpd_req_t *); } store_job_t;

static void store_task(void *arg)
{
    store_job_t job;
    for (;;) {
        if (xQueueReceive(store_queue, &job, portMAX_DELAY) == pdTRUE) job.work(job.req);
    }
}

static esp_err_t hand_to_store(httpd_req_t *req, void (*work)(httpd_req_t *))
{
    if (!uxQueueSpacesAvailable(store_queue)) return refuse(req, 409, "the card is busy");
    store_job_t job = {.work = work};
    if (httpd_req_async_handler_begin(req, &job.req) != ESP_OK) return refuse(req, 500, "could not take it");
    xQueueSend(store_queue, &job, 0);
    return ESP_OK;
}

static void ota_yield(void)
{
    while (wake_busy()) vTaskDelay(pdMS_TO_TICKS(VOICE_FIRST_MS));   // never while she is being heard
}

// PUT /firmware: her program over the air, written by ota.c. On its OWN task
// with an internal stack: a flash write runs with the cache off, and the store
// task's stack is in PSRAM, unreachable then (IDF asserts on it; 2026-10-07).
static void ota_task(void *arg)
{
    ota_put((httpd_req_t *)arg, STORE_PACE_MS, ota_yield);
    ota_running = false;
    vTaskDelete(NULL);
}

static esp_err_t on_firmware(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (ota_running || storage_busy()) return refuse(req, 409, "busy: a program or a backup is arriving");
    httpd_req_t *held;
    if (httpd_req_async_handler_begin(req, &held) != ESP_OK) return refuse(req, 500, "could not take the program");
    ota_running = true;
    if (xTaskCreatePinnedToCore(ota_task, "ota", 8192, held, 4, NULL, 1) != pdPASS) {
        ota_running = false;
        refuse(held, 500, "no memory for the update task");
        httpd_req_async_handler_complete(held);
    }
    return ESP_OK;
}

// --- the eye: GET /camera/snap?b64=true&size=800x600 ------------------------------------------
// {"width", "height", "sensor", "bytes", "data_b64"}: the JPEG as base64, streamed so no
// 150 KB string is built. A shot takes a few seconds (the warning, the exposure): on its
// own short task with an internal stack, like the OTA.

static volatile bool snapping;

static void snap_task(void *arg)
{
    httpd_req_t *req = arg;
    char text[24];
    int w = 0, h = 0;
    if (query_value(req, "size", text, sizeof text)) sscanf(text, "%dx%d", &w, &h);
    snap_t shot;
    char why[96] = {0};
    uint8_t *b64 = NULL;
    if (!camera_snap(w, h, &shot, why, sizeof why)) { refuse(req, 503, why); goto done; }
    size_t need = 0, len = 0;
    mbedtls_base64_encode(NULL, 0, &need, shot.jpeg, shot.len);          // how much, incl. the NUL
    b64 = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!b64 || mbedtls_base64_encode(b64, need, &len, shot.jpeg, shot.len) != 0) {
        refuse(req, 500, "no memory for the picture");
        goto done;
    }
    httpd_resp_set_type(req, "application/json");
    char head[160];
    snprintf(head, sizeof head, "{\"width\": %d, \"height\": %d, \"sensor\": \"%s\", \"bytes\": %u, \"data_b64\": \"",
             shot.w, shot.h, camera_sensor_name(), (unsigned)shot.len);
    bool sent = httpd_resp_send_chunk(req, head, strlen(head)) == ESP_OK;
    for (size_t at = 0; sent && at < len; at += 8192)
        sent = httpd_resp_send_chunk(req, (char *)b64 + at, len - at < 8192 ? len - at : 8192) == ESP_OK;
    if (sent) { httpd_resp_send_chunk(req, "\"}", 2); httpd_resp_send_chunk(req, NULL, 0); }
done:
    free(b64);
    camera_snap_done(&shot);
    httpd_req_async_handler_complete(req);
    snapping = false;
    vTaskDelete(NULL);
}

static esp_err_t on_snap(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!camera_present()) return refuse(req, 404, "this board has no camera");
    if (snapping) return refuse(req, 409, "a picture is already being taken");
    httpd_req_t *held;
    if (httpd_req_async_handler_begin(req, &held) != ESP_OK) return refuse(req, 500, "could not take the request");
    snapping = true;
    if (xTaskCreatePinnedToCore(snap_task, "snap", 8192, held, 4, NULL, 1) != pdPASS) {
        snapping = false;
        refuse(held, 500, "no memory for the camera task");
        httpd_req_async_handler_complete(held);
    }
    return ESP_OK;
}

// --- the glass: /screen, /screen/picture, /screen/background, /screen/power ----------------
// An image body is w x h pixels of RGB565 big-endian (query w, h <= the
// screen). It lands in a PSRAM buffer the glass gives out, then is shown: a
// picture for `seconds` or until a tap, a background until the next one.

#define PICTURE_DEFAULT_S 60

#define GLASS_STUCK "the glass did not answer in half a second (it restarts itself after 10)"
#define JPEG_MAX (256 * 1024)          // one picture as JPEG: the whole glass at any quality sits well under it

// ?format=jpeg: the body is one JPEG (its own size, w and h multiples of 16, up to
// the screen's). It is read whole into PSRAM, then decoded straight into the
// buffer the glass gives out, as the RGB565 big-endian a raw picture would be.
// Answers false when it was refused (and says why); else *w, *h are its size.
static struct { int recv_ms, wait_ms, decode_ms, internal_kb; } jpeg_took;   // the last JPEG's costs, said in its answer

static bool jpeg_work(httpd_req_t *req, int kind, int *w, int *h)
{
    bool done = false;
    int64_t t0 = esp_timer_get_time();
    jpeg_dec_handle_t dec = NULL;
    size_t len = req->content_len;
    uint8_t *in = len && len <= JPEG_MAX ? heap_caps_malloc(len, MALLOC_CAP_SPIRAM) : NULL;
    if (!in) { refuse(req, len > JPEG_MAX ? 413 : 400, len ? "the JPEG is too big or there is no memory" : "no JPEG"); return false; }
    for (size_t have = 0; have < len;) {
        int n = httpd_req_recv(req, (char *)in + have, len - have > 8192 ? 8192 : len - have);
        if (n <= 0) { refuse(req, 400, "the JPEG did not arrive whole"); goto out; }
        have += n;
    }
    int64_t t1 = esp_timer_get_time();
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    jpeg_dec_io_t io = {.inbuf = in, .inbuf_len = len};
    jpeg_dec_header_info_t info;
    int out_len = 0;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK || jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        refuse(req, 400, "that is not a JPEG this board reads"); goto out;
    }
    *w = info.width; *h = info.height;
    if (*w > BOARD_W || *h > BOARD_H || *w % 16 || *h % 16 || jpeg_dec_get_outbuf_len(dec, &out_len) != JPEG_ERR_OK
        || out_len != *w * *h * 2) {
        refuse(req, 400, "a JPEG up to the screen's size, width and height multiples of 16"); goto out;
    }
    io.outbuf = ui_image_begin(kind, *w, *h);            // waits while the glass draws the last frame
    if (!io.outbuf) { refuse(req, 503, "the glass is busy or out of memory"); goto out; }
    int64_t t2 = esp_timer_get_time();
    jpeg_took.internal_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;   // while the decoder is open
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) { ui_image_abort(kind); refuse(req, 400, "the JPEG would not decode"); goto out; }
    jpeg_took.recv_ms = (t1 - t0) / 1000;
    jpeg_took.wait_ms = (t2 - t1) / 1000;
    jpeg_took.decode_ms = (esp_timer_get_time() - t2) / 1000;
    done = true;
out:
    if (dec) jpeg_dec_close(dec);
    free(in);
    return done;
}

static void say_hidden(httpd_req_t *req)                 // a frame taken and dropped: tapped away, or a game left by a long press
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "hidden", true);
    say_json(req, 200, j);
}

static void image_work(httpd_req_t *req)
{
    bool background = !strcmp(req->uri, "/screen/background") || !strncmp(req->uri, "/screen/background?", 19);
    int kind = background ? UI_SCENE : UI_PICTURE;
    char text[64];
    int w = query_value(req, "w", text, sizeof text) ? atoi(text) : 0;
    int h = query_value(req, "h", text, sizeof text) ? atoi(text) : 0;
    int seconds = query_value(req, "seconds", text, sizeof text) ? atoi(text) : PICTURE_DEFAULT_S;
    char name[64] = {0};
    bool replaced = false;                                      // a frame: it took the place of one still waiting for the beat
    bool taps = query_value(req, "taps", text, sizeof text) && (text[0] == '1' || text[0] == 't');   // the sender wants the taps (0.9.0)
    if (query_value(req, "name", text, sizeof text)) { unescape(text); strlcpy(name, text, sizeof name); }
    if (query_value(req, "format", text, sizeof text) && !strcmp(text, "jpeg")) {
        bool left = kind == UI_PICTURE && taps && ui_stream_left();   // asked every time, so a hold's "left" is consumed by this game's next frame and never waits for the next game's (0.9.3)
        if (kind == UI_PICTURE && (ui_stream_hidden() || left)) {   // tapped away, or a game left by a long press (its own frames only): taken and dropped, not decoded
            char sink[512];
            for (size_t have = 0; have < req->content_len;) {
                int n = httpd_req_recv(req, sink, req->content_len - have > sizeof sink ? sizeof sink : req->content_len - have);
                if (n <= 0) break;
                have += n;
            }
            say_hidden(req);
            goto done;
        }
        if (!jpeg_work(req, kind, &w, &h)) goto done;
        if (kind == UI_PICTURE) goto frame;
        goto show;
    }
    if (w < 1 || h < 1 || w > BOARD_W || h > BOARD_H || req->content_len != (size_t)w * h * 2) {
        refuse(req, 400, "w and h up to the screen's, and exactly w*h*2 bytes of RGB565");
        goto done;
    }
    uint8_t *px = ui_image_begin(kind, w, h);
    if (!px) { refuse(req, 503, "the glass is busy or out of memory"); goto done; }
    size_t have = 0, want = (size_t)w * h * 2;
    while (have < want) {
        int n = httpd_req_recv(req, (char *)px + have, want - have > 8192 ? 8192 : want - have);
        if (n <= 0) { ui_image_abort(kind); refuse(req, 400, "the image did not arrive whole"); goto done; }
        have += n;
    }
    int64_t shown_at;
show:
    shown_at = esp_timer_get_time();
    if (!ui_image_show(kind, w, h, seconds, name)) { refuse(req, 503, GLASS_STUCK); goto done; }
    goto shown;
frame:
    shown_at = esp_timer_get_time();
    if (!ui_frame_ready(w, h, seconds, taps, &replaced)) {                                            // painted on the beat (ui.c), answered now
        if (ui_stream_hidden()) { say_hidden(req); goto done; }                                       // tapped away, or left by a hold, while this one decoded: dropped
        refuse(req, 503, GLASS_STUCK); goto done;
    }
shown:;
    int show_ms = (esp_timer_get_time() - shown_at) / 1000;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    if (background) cJSON_AddStringToObject(j, "background", name);
    else cJSON_AddNumberToObject(j, "seconds", seconds);
    if (query_value(req, "format", text, sizeof text) && !strcmp(text, "jpeg")) {
        cJSON *took = cJSON_AddObjectToObject(j, "took_ms");      // where a frame's time goes
        cJSON_AddNumberToObject(took, "recv", jpeg_took.recv_ms);
        cJSON_AddNumberToObject(took, "wait", jpeg_took.wait_ms);
        cJSON_AddNumberToObject(took, "decode", jpeg_took.decode_ms);
        cJSON_AddNumberToObject(took, "show", show_ms);           // a frame: handed to the beat; a background: painted
        cJSON_AddNumberToObject(j, "internal_kb_decoding", jpeg_took.internal_kb);
        if (kind == UI_PICTURE) {
            cJSON_AddNumberToObject(j, "fps", UI_STREAM_FPS);     // the beat it is painted on: a sender paces to it
            cJSON_AddBoolToObject(j, "replaced", replaced);       // true: the sender is ahead of the beat
        }
    }
    say_json(req, 200, j);
done:
    httpd_req_async_handler_complete(req);
}

static void glass_task(void *arg)
{
    httpd_req_t *req;
    for (;;) {
        if (xQueueReceive(glass_queue, &req, portMAX_DELAY) == pdTRUE) image_work(req);
    }
}

static esp_err_t on_image(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!ui_ok()) return refuse(req, 503, "this board has no screen up");
    char text[16];
    if (!query_value(req, "w", text, sizeof text) && !query_value(req, "format", text, sizeof text)) {   // no size, no JPEG: a clear, as JSON {"clear": true} or an empty body
        if (req->content_len) { cJSON *in = read_json(req); if (!in) return ESP_OK; cJSON_Delete(in); }
        if (!ui_image_drop(!strncmp(req->uri, "/screen/background", 18) ? UI_SCENE : UI_PICTURE)) return refuse(req, 503, GLASS_STUCK);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        return say_json(req, 200, j);
    }
    if (!uxQueueSpacesAvailable(glass_queue)) return refuse(req, 409, "an image is already arriving");
    httpd_req_t *held;
    if (httpd_req_async_handler_begin(req, &held) != ESP_OK) return refuse(req, 500, "could not take the image");
    xQueueSend(glass_queue, &held, 0);
    return ESP_OK;
}

static esp_err_t on_screen(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!ui_ok()) return refuse(req, 503, "this board has no screen up");
    cJSON *in = read_json(req);
    if (!in) return ESP_OK;
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    bool shown = true;
    const cJSON *chat = cJSON_GetObjectItem(in, "chat");
    if (cJSON_IsString(chat)) {                                    // {"chat": "default"}: the name at the bottom; "" hides it
        const cJSON *brain = cJSON_GetObjectItem(in, "brain");     // with it: the model's name and "#rrggbb", the chat's trim
        const cJSON *tint = cJSON_GetObjectItem(in, "trim");
        int rgb = -1;
        if (cJSON_IsString(tint) && tint->valuestring[0] == '#' && strlen(tint->valuestring) == 7) rgb = (int)strtol(tint->valuestring + 1, NULL, 16);
        shown = ui_chat(chat->valuestring, cJSON_IsString(brain) ? brain->valuestring : "", rgb);
        cJSON_AddStringToObject(out, "chat", chat->valuestring);
    } else if (cJSON_IsTrue(cJSON_GetObjectItem(in, "clear"))) {
        shown = ui_toast("", 0);
    } else {
        const cJSON *text = cJSON_GetObjectItem(in, "text");
        const cJSON *secs = cJSON_GetObjectItem(in, "seconds");
        if (!cJSON_IsString(text) || !text->valuestring[0]) { cJSON_Delete(in); cJSON_Delete(out); return refuse(req, 400, "text is needed"); }
        int seconds = cJSON_IsNumber(secs) ? (int)secs->valuedouble : 20;
        shown = ui_toast(text->valuestring, seconds);
        cJSON_AddNumberToObject(out, "seconds", seconds);
    }
    cJSON_Delete(in);
    if (!shown) { cJSON_Delete(out); return refuse(req, 503, GLASS_STUCK); }
    return say_json(req, 200, out);
}

// PUT /screen/settings {"brightness", "dim", "dim_after_s", "off_after_min", "flip"}: any of them; kept across a restart.
// The backlight is the board's biggest draw that can be chosen: bright while in use, a glow or dark when left alone.
static esp_err_t on_screen_settings(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!ui_ok()) return refuse(req, 503, "this board has no screen up");
    cJSON *in = read_json(req);
    if (!in) return ESP_OK;
    static const struct { const char *key; int *at; int max; } fields[] = {
        {"brightness", &settings.brightness, 100}, {"dim", &settings.dim_percent, 100},
        {"dim_after_s", &settings.dim_after_s, 3600}, {"off_after_min", &settings.off_after_min, 1440},
    };
    bool changed = false;
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        const cJSON *v = cJSON_GetObjectItem(in, fields[i].key);
        if (!cJSON_IsNumber(v)) continue;
        int n = (int)v->valuedouble;
        n = n < 0 ? 0 : n > fields[i].max ? fields[i].max : n;
        if (i == 0 && n < 5) n = 5;                             // a glass at 0 is black and its buttons still act: "off" is /screen/power
        if (n != *fields[i].at) { *fields[i].at = n; changed = true; }
    }
    const cJSON *flip = cJSON_GetObjectItem(in, "flip");       // 180 degrees, for a board mounted the other way up
    if (cJSON_IsBool(flip) && cJSON_IsTrue(flip) != settings.flip) {
        settings.flip = cJSON_IsTrue(flip);
        board_glass_orient(settings.invert, settings.flip);
        changed = true;
    }
    cJSON_Delete(in);
    if (changed) { settings_save(); ui_relight(); }
    cJSON *j = cJSON_CreateObject();
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) cJSON_AddNumberToObject(j, fields[i].key, *fields[i].at);
    cJSON_AddBoolToObject(j, "flip", settings.flip);
    return say_json(req, 200, j);
}

// POST /screen/power?on=true|false, GET /screen/power
static esp_err_t on_screen_power(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!ui_ok()) return refuse(req, 503, "this board has no screen up");
    char text[8];
    if (req->method == HTTP_POST) {
        if (!query_value(req, "on", text, sizeof text)) return refuse(req, 400, "on=true or false");
        if (!ui_screen_power(text[0] == 't' || text[0] == '1')) return refuse(req, 503, GLASS_STUCK);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "on", ui_screen_on());
    return say_json(req, 200, j);
}

// POST /mute?speaker=on|off, GET /mute: the speaker's mute (the wake word's is /wakeword)
static esp_err_t on_mute(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    char text[8];
    if (req->method == HTTP_POST) {
        if (!query_value(req, "speaker", text, sizeof text)) return refuse(req, 400, "speaker=on or off");
        audio_set_mute(text[0] == 'o' ? text[1] == 'n' : (text[0] == '1' || text[0] == 't'));
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "speaker", audio_muted());
    return say_json(req, 200, j);
}

static esp_err_t on_storage_file(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!storage_alive()) return refuse(req, 503, "no card");
    const char *name = file_name(req);
    if (!storage_name_ok(name)) return refuse(req, 400, "bad name");
    if (req->method == HTTP_DELETE) {
        if (!storage_remove(name)) return refuse(req, 404, "no such file, or the card is busy");
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        return say_json(req, 200, j);
    }
    if (req->method == HTTP_GET) return hand_to_store(req, store_get);
    // PUT
    if (!req->content_len) return refuse(req, 411, "Content-Length required");
    size_t n = strlen(name);
    bool sealed_name = n > 12 && !strcmp(name + n - 12, ".sapphirebak");
    if (!sealed_name && !storage_name_plain_ok(name)) return refuse(req, 400, "only sealed backups (.sapphirebak) land here");
    uint64_t free_b = 0;
    if (storage_room(&free_b, NULL) && (uint64_t)req->content_len + (16u << 20) > free_b)
        return refuse(req, 507, "not enough room on the card");
    if (storage_busy()) return refuse(req, 409, "another backup is arriving");
    if (ota_running) return refuse(req, 409, "a program is arriving: the board restarts when it has");
    return hand_to_store(req, store_put);
}

static esp_err_t on_storage_format(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!storage_ok()) return refuse(req, 503, "no card");
    if (storage_busy()) return refuse(req, 409, "a backup is arriving");
    return hand_to_store(req, store_format);
}

// --- start ------------------------------------------------------------------------------

void server_start(void)
{
    if (server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 6144;                    // internal RAM (its handlers save settings to flash); 2.8 KB is the most it has used
    cfg.max_uri_handlers = 48;
    // 5 and the server's own 3 of LWIP's 16: Sapphire keeps her connections open, and at the
    // default 7 of 10 she could hold every socket the board has, with none left for its own
    // question to her. Past 5 the least used idle one is closed (lru_purge_enable).
    cfg.max_open_sockets = 5;
    cfg.core_id = 1;                                    // core 0 is WiFi + AFE: saturated while a backup lands (2026-10-06)
    cfg.uri_match_fn = httpd_uri_match_wildcard;        // /storage/* ; every other route is still exact
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) != ESP_OK) { ESP_LOGE(TAG, "the server did not start"); return; }
    speak_queue = xQueueCreate(2, sizeof(httpd_req_t *));
    play_queue = xQueueCreate(8, sizeof(sentence_t));
    task_in_psram(play_task, "play", 6144, 5, 1);       // plays what speak took in, from PSRAM
    task_in_psram(speak_task, "speak", 8192, 5, 1);      // plays sound, writes no flash: +8 KB of internal RAM (0.4.0)
    listen_queue = xQueueCreate(1, sizeof(listen_job_t));
    task_in_psram(listen_task, "listen", 6144, 5, 1);    // a listen waits for her to finish, off the server task
    glass_queue = xQueueCreate(1, sizeof(httpd_req_t *));
    task_in_psram(glass_task, "glass", 8192, 4, 1);
    store_queue = xQueueCreate(1, sizeof(store_job_t));
    task_in_psram(store_task, "store", 8192, 3, 1);   // under the server and the ring, off the WiFi core; card writes are not flash writes
    const httpd_uri_t routes[] = {
        {"/health", HTTP_GET, on_health, NULL},
        {"/sensors", HTTP_GET, on_sensors, NULL},
        {"/audio/speak", HTTP_POST, on_speak, NULL},
        {"/audio/stop", HTTP_POST, on_stop, NULL},
        {"/audio/listen", HTTP_GET, on_listen, NULL},
        {"/capture", HTTP_GET, on_capture, NULL},
        {"/capture", HTTP_POST, on_capture, NULL},
        {"/capture", HTTP_DELETE, on_capture, NULL},
        {"/wakeword", HTTP_GET, on_wakeword, NULL},
        {"/wakeword", HTTP_POST, on_wakeword, NULL},
        {"/wakeword/model", HTTP_PUT, on_wakeword_model, NULL},
        {"/wakeword/model", HTTP_DELETE, on_wakeword_model, NULL},
        {"/power", HTTP_POST, on_power, NULL},
        {"/sounds", HTTP_GET, on_sounds, NULL},
        {"/volume", HTTP_GET, on_volume, NULL},
        {"/volume", HTTP_POST, on_volume, NULL},
        {"/mic", HTTP_GET, on_mic, NULL},
        {"/mic", HTTP_POST, on_mic, NULL},
        {"/led", HTTP_POST, on_led, NULL},
        {"/led/spec", HTTP_GET, on_led_spec, NULL},
        {"/led/baseline", HTTP_GET, on_led_baseline, NULL},
        {"/led/baseline", HTTP_PUT, on_led_baseline, NULL},
        {"/led/looks", HTTP_PUT, on_led_looks, NULL},
        {"/screen", HTTP_POST, on_screen, NULL},
        {"/screen/picture", HTTP_POST, on_image, NULL},
        {"/screen/background", HTTP_POST, on_image, NULL},
        {"/screen/settings", HTTP_PUT, on_screen_settings, NULL},
        {"/screen/power", HTTP_GET, on_screen_power, NULL},
        {"/screen/power", HTTP_POST, on_screen_power, NULL},
        {"/mute", HTTP_GET, on_mute, NULL},
        {"/mute", HTTP_POST, on_mute, NULL},
        {"/camera/snap", HTTP_GET, on_snap, NULL},
        // exact ones before the wildcard: the first match wins
        {"/storage", HTTP_GET, on_storage, NULL},
        {"/firmware", HTTP_PUT, on_firmware, NULL},
        {"/storage/format", HTTP_POST, on_storage_format, NULL},
        {"/storage/check", HTTP_POST, on_storage_check, NULL},
        {"/storage/*", HTTP_PUT, on_storage_file, NULL},
        {"/storage/*", HTTP_GET, on_storage_file, NULL},
        {"/storage/*", HTTP_DELETE, on_storage_file, NULL},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) httpd_register_uri_handler(server, &routes[i]);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, on_missing);
    ESP_LOGI(TAG, "listening on port 80");
}
