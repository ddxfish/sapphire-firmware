// link.c - see link.h.
#include "link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "audio.h"
#include "buttons.h"
#include "light.h"
#include "net.h"
#include "server.h"
#include "settings.h"
#include "ui.h"
#include "wake.h"
#include "wav.h"

static const char *TAG = "link";
static bool connected;
static int last_refusal;               // the events door's last status, said once
#define VOICE_WAIT_MS 90000            // her answer comes once the words are known: a minute of talk takes a while to hear
#define CLAIM_WAIT_MS 3000             // the room is decided in a moment, or not at all
#define EVENTS_QUIET_MS 45000          // a comment line arrives every 20 s
#define EVENT_LINE 1024
#define VOICE_ANSWER 8192              // the voice door's answer, with all she heard in it
#define PRESS_WAIT_MS 15000            // answered once a turn has started
#define ROOM_WAIT_MS 20000             // a piece of the room is a megabyte: seconds over WiFi, never a minute

typedef struct { char button[16], how[8], job[16]; } press_t;   // job set: not a press, work asked of Sapphire (link_ask)
static QueueHandle_t presses;

static void url_for(const char *door, char *out, size_t size)
{
    snprintf(out, size, "%s/api/devices/%s/%s", settings.sapphire, settings.name, door);
}

static esp_http_client_handle_t open_client(const char *door, esp_http_client_method_t method, int timeout_ms)
{
    char url[SETTING_MAX + 128], bearer[SETTING_MAX + 8];
    url_for(door, url, sizeof url);
    snprintf(bearer, sizeof bearer, "Bearer %s", settings.voice_key);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .cert_pem = settings.cert[0] ? settings.cert : NULL,
        .skip_cert_common_name_check = true,      // her certificate names no address
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) esp_http_client_set_header(c, "Authorization", bearer);
    return c;
}

// --- what it heard ------------------------------------------------------------------

bool link_send_voice(const int16_t *pcm, size_t samples, char *heard, size_t heard_size)
{
    heard[0] = 0;
    if (!net_up()) { ESP_LOGW(TAG, "no WiFi: the question is lost"); return false; }
    // found within CLAIM_WAIT_MS x 3 or not at all; the long wait is for her answer. With one
    // timeout for both, a host that was switched off held the board deaf for a minute and a half.
    esp_http_client_handle_t c = open_client("voice", HTTP_METHOD_POST, CLAIM_WAIT_MS * 3);
    if (!c) return false;
    esp_http_client_set_header(c, "Content-Type", "audio/wav");
    size_t bytes = samples * 2;
    bool ok = false;
    if (esp_http_client_open(c, WAV_HEADER + bytes) == ESP_OK) {
        esp_http_client_set_timeout_ms(c, VOICE_WAIT_MS);
        uint8_t head[WAV_HEADER];
        wav_header(head, AUDIO_RATE, 1, bytes);
        bool sent = esp_http_client_write(c, (char *)head, WAV_HEADER) == WAV_HEADER;
        for (size_t at = 0; sent && at < bytes; at += 4096) {
            size_t n = bytes - at < 4096 ? bytes - at : 4096;
            sent = esp_http_client_write(c, (const char *)pcm + at, n) == (int)n;
        }
        if (sent && esp_http_client_fetch_headers(c) >= 0) {
            int status = esp_http_client_get_status_code(c);
            // her answer carries every word she heard: a minute of talk is past 1 KB, and cut short
            // it did not parse, so a question she took showed as an error (scout, 2026-10-11)
            char *body = heap_caps_calloc(1, VOICE_ANSWER, MALLOC_CAP_SPIRAM);
            int n = body ? esp_http_client_read_response(c, body, VOICE_ANSWER - 1) : 0;
            if (n > 0) body[n] = 0;
            cJSON *j = body ? cJSON_Parse(body) : NULL;
            cJSON *h = j ? cJSON_GetObjectItem(j, "heard") : NULL;
            cJSON *e = j ? cJSON_GetObjectItem(j, "error") : NULL;
            if (cJSON_IsString(h)) strlcpy(heard, h->valuestring, heard_size);
            ok = status == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "ok"));
            if (!ok) ESP_LOGW(TAG, "voice door answered %d: %.200s", status,
                              cJSON_IsString(e) ? e->valuestring : body ? body : "");
            cJSON_Delete(j);
            free(body);
        } else {
            ESP_LOGW(TAG, "the question did not reach Sapphire");
        }
    } else {
        ESP_LOGW(TAG, "could not reach Sapphire at %s", settings.sapphire);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

// --- a piece of the room (capture.c) ---------------------------------------------------

int link_send_room(const int16_t *pcm, size_t samples, const char *capture_id, int chunk, const char *fires,
                   int n_fires, bool test, const char *model)
{
    if (!net_up()) return 0;
    esp_http_client_handle_t c = open_client("room", HTTP_METHOD_POST, ROOM_WAIT_MS);
    if (!c) return 0;
    char text[16];
    esp_http_client_set_header(c, "Content-Type", "audio/wav");
    esp_http_client_set_header(c, "X-Capture", capture_id);
    snprintf(text, sizeof text, "%d", chunk);
    esp_http_client_set_header(c, "X-Chunk", text);
    esp_http_client_set_header(c, "X-Fires", fires);
    char count[16];
    snprintf(count, sizeof count, "%d", n_fires);
    esp_http_client_set_header(c, "X-Fire-Count", count);
    esp_http_client_set_header(c, "X-Test", test ? "1" : "0");
    esp_http_client_set_header(c, "X-Model", model ? model : "");
    size_t bytes = samples * 2;
    int status = 0;
    if (esp_http_client_open(c, WAV_HEADER + bytes) == ESP_OK) {
        uint8_t head[WAV_HEADER];
        wav_header(head, AUDIO_RATE, 1, bytes);
        bool ok = esp_http_client_write(c, (char *)head, WAV_HEADER) == WAV_HEADER;
        for (size_t at = 0; ok && at < bytes; at += 4096) {
            size_t n = bytes - at < 4096 ? bytes - at : 4096;
            ok = esp_http_client_write(c, (const char *)pcm + at, n) == (int)n;
        }
        if (ok && esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
            char body[256] = {0};
            int n = esp_http_client_read_response(c, body, sizeof body - 1);
            if (n > 0) body[n] = 0;
            if (status == 200) {
                cJSON *j = cJSON_Parse(body);
                if (j && cJSON_IsFalse(cJSON_GetObjectItem(j, "accepted"))) status = 204;   // taken, nobody collecting
                else if (j && cJSON_IsFalse(cJSON_GetObjectItem(j, "ok"))) status = 500;     // her side had trouble: try the next piece
                cJSON_Delete(j);
            } else {
                ESP_LOGW(TAG, "room door answered %d: %.160s", status, body);
            }
        } else {
            ESP_LOGW(TAG, "a piece of the room did not reach Sapphire");
        }
    } else {
        ESP_LOGW(TAG, "could not reach Sapphire at %s", settings.sapphire);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

// --- the wake: may this board take it? -------------------------------------------------

bool link_claim_wake(char *taken_by, size_t size)
{
    taken_by[0] = 0;
    if (!net_up()) return true;
    esp_http_client_handle_t c = open_client("wake", HTTP_METHOD_POST, CLAIM_WAIT_MS);
    if (!c) return true;
    bool mine = true;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        char body[256] = {0};
        int n = esp_http_client_read_response(c, body, sizeof body - 1);
        if (n > 0) body[n] = 0;
        cJSON *j = cJSON_Parse(body);
        if (esp_http_client_get_status_code(c) == 200 && j) {
            const cJSON *who = cJSON_GetObjectItem(j, "taken_by");
            if (cJSON_IsFalse(cJSON_GetObjectItem(j, "yours"))) {
                mine = false;
                if (cJSON_IsString(who)) strlcpy(taken_by, who->valuestring, size);
            }
        }
        cJSON_Delete(j);
    } else {
        ESP_LOGW(TAG, "the wake could not be claimed; going on with it");
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return mine;
}

// --- a button set to reach her ---------------------------------------------------------

static void send_press(const press_t *p)
{
    esp_http_client_handle_t c = open_client("press", HTTP_METHOD_POST, PRESS_WAIT_MS);
    if (!c) { light_cue("error"); return; }
    char out[64], body[512] = {0};
    int len = snprintf(out, sizeof out, "{\"button\": \"%s\", \"how\": \"%s\"}", p->button, p->how);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    bool ok = false, taken = false;
    if (esp_http_client_open(c, len) == ESP_OK && esp_http_client_write(c, out, len) == len
            && esp_http_client_fetch_headers(c) >= 0) {
        int n = esp_http_client_read_response(c, body, sizeof body - 1);
        if (n > 0) body[n] = 0;
        cJSON *j = cJSON_Parse(body);
        ok = esp_http_client_get_status_code(c) == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "ok"));
        taken = ok && cJSON_IsTrue(cJSON_GetObjectItem(j, "accepted"));
        const cJSON *e = j ? cJSON_GetObjectItem(j, "error") : NULL;
        if (!ok) ESP_LOGW(TAG, "press door answered %d: %s", esp_http_client_get_status_code(c),
                          cJSON_IsString(e) ? e->valuestring : body);
        cJSON_Delete(j);
    } else {
        ESP_LOGW(TAG, "the press did not reach Sapphire");
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!ok) light_cue("error");
    else if (!taken) light_cue("idle");          // nothing is set for it on her side: no turn, no cue will come
}

// --- work that is Sapphire's: POST .../ask {"job": "stop" | "backup"} -------------------
// Her answer carries `said`, a line for a small screen. A stop says nothing when it went well.

static void send_ask(const char *job)
{
    char out[48], body[384] = {0}, why[96] = "Sapphire did not answer";
    int len = snprintf(out, sizeof out, "{\"job\": \"%s\"}", job);
    bool ok = false;
    esp_http_client_handle_t c = open_client("ask", HTTP_METHOD_POST, PRESS_WAIT_MS);
    if (c) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        if (esp_http_client_open(c, len) == ESP_OK && esp_http_client_write(c, out, len) == len
                && esp_http_client_fetch_headers(c) >= 0) {
            int n = esp_http_client_read_response(c, body, sizeof body - 1);
            if (n > 0) body[n] = 0;
            int status = esp_http_client_get_status_code(c);
            cJSON *j = cJSON_Parse(body);
            const cJSON *said = j ? cJSON_GetObjectItem(j, "said") : NULL;
            const cJSON *e = j ? cJSON_GetObjectItem(j, "error") : NULL;
            if (j && !e) e = cJSON_GetObjectItem(j, "detail");
            ok = status == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "ok"));
            if (ok && cJSON_IsString(said)) strlcpy(why, said->valuestring, sizeof why);
            else if (cJSON_IsString(e)) strlcpy(why, e->valuestring, sizeof why);
            else if (status == 404) strlcpy(why, "Sapphire is too old for this: update her", sizeof why);
            else snprintf(why, sizeof why, "Sapphire answered %d", status);
            cJSON_Delete(j);
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    ESP_LOGI(TAG, "asked for %s: %s", job, why);
    if (!ok || strcmp(job, "stop")) ui_toast(why, ok ? 6 : 10);
}

static void press_task(void *arg)
{
    press_t p;
    for (;;)
        if (xQueueReceive(presses, &p, portMAX_DELAY) == pdTRUE) {
            if (p.job[0]) send_ask(p.job);
            else send_press(&p);
        }
}

void link_ask(const char *job)
{
    press_t p = {0};
    strlcpy(p.job, job, sizeof p.job);
    if (!presses || !net_up() || xQueueSend(presses, &p, 0) != pdTRUE) {
        if (strcmp(job, "stop")) ui_toast("No link to Sapphire", 6);      // a stop has already gone quiet here
    }
}

void link_press(const char *button, const char *how)
{
    press_t p = {0};
    strlcpy(p.button, button, sizeof p.button);
    strlcpy(p.how, how, sizeof p.how);
    if (!presses || !net_up() || xQueueSend(presses, &p, 0) != pdTRUE) { light_cue("error"); return; }
    light_cue("thinking");                       // at once: her own cue follows when the turn starts
}

// --- what the light should show ------------------------------------------------------

static void take_line(const char *line)
{
    if (strncmp(line, "data: ", 6)) return;                 // ": still here" and blank lines
    cJSON *j = cJSON_Parse(line + 6);
    cJSON *state = j ? cJSON_GetObjectItem(j, "state") : NULL;
    if (cJSON_IsString(state)) {
        if (!strcmp(state->valuestring, "connected")) {
            connected = true;
            last_refusal = 0;
            // Sapphire's clock and time zone ride along: the board keeps
            // local time for the ring's active hours without an internet clock
            const cJSON *now = cJSON_GetObjectItem(j, "now");
            const cJSON *tz = cJSON_GetObjectItem(j, "tz");
            if (cJSON_IsString(tz) && tz->valuestring[0]) { setenv("TZ", tz->valuestring, 1); tzset(); }
            if (cJSON_IsNumber(now)) {
                struct timeval tv = {.tv_sec = (time_t)now->valuedouble};
                settimeofday(&tv, NULL);
            }
            ESP_LOGI(TAG, "events stream open%s", cJSON_IsNumber(now) ? ", clock set" : "");
            server_reply_over();                 // whatever was stopped here ended while the stream was away
            light_cue_from_brain("idle");
        }
        else if (!strcmp(state->valuestring, "standdown")) wake_standdown("Sapphire's own microphone");
        else if (!strcmp(state->valuestring, "buttons")) buttons_bound(cJSON_GetObjectItem(j, "bound"));
        else {
            const char *s = state->valuestring;
            if (!strcmp(s, "idle") || !strcmp(s, "error")) server_reply_over();   // her turn ended: a reply stopped here is done with
            // a word this program does not know (a newer Sapphire's) is not a look: it was painted as one
            if (!strcmp(s, "thinking") || !strcmp(s, "tool") || !strcmp(s, "idle") || !strcmp(s, "error")) light_cue_from_brain(s);
        }
    }
    cJSON_Delete(j);
}

static void hold_events(void)
{
    esp_http_client_handle_t c = open_client("events", HTTP_METHOD_GET, EVENTS_QUIET_MS);
    if (!c) return;
    ESP_LOGD(TAG, "events: opening");
    esp_err_t opened = esp_http_client_open(c, 0);
    int64_t length = opened == ESP_OK ? esp_http_client_fetch_headers(c) : -1;
    ESP_LOGD(TAG, "events: open %s, headers %lld, status %d", esp_err_to_name(opened), (long long)length,
             esp_http_client_get_status_code(c));
    if (opened == ESP_OK && length >= 0 && esp_http_client_get_status_code(c) == 200) {
        // One byte at a time: esp_http_client_read() only returns when the
        // buffer it was given is full or the stream ends, and this stream
        // sends a few bytes every 20 s. The stream is tiny, the cost is nothing.
        char line[EVENT_LINE];
        size_t have = 0;
        for (;;) {
            char ch;
            int n = esp_http_client_read(c, &ch, 1);
            if (n <= 0) {                                  // closed, or quiet too long
                ESP_LOGW(TAG, "events stream ended (%d)", n);
                break;
            }
            if (ch == '\n') {
                line[have] = 0;
                take_line(line);
                have = 0;
            } else if (have < sizeof line - 1) {
                line[have++] = ch;
            }
        }
    } else {
        int status = esp_http_client_get_status_code(c);
        if (status != last_refusal) {              // said once, not every few seconds
            last_refusal = status;
            ESP_LOGW(TAG, "events stream refused: %d%s", status,
                     status == 401 ? " (not added in Settings > Devices yet, or a wrong key)" : "");
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    ESP_LOGD(TAG, "events: closed");
}

static void events_task(void *arg)
{
    int pause = 1;
    for (;;) {
        if (net_up()) {
            hold_events();
            if (connected) pause = 1;                      // it had been open: back at once, not after the longest wait
            connected = false;
            light_cue("nolink");
            ESP_LOGD(TAG, "events: again in %d s", pause);
        }
        vTaskDelay(pdMS_TO_TICKS(pause * 1000));
        pause = pause < 30 ? pause * 2 : 30;
        if (net_up() && pause > 8) pause = 8;              // WiFi is fine: it is her that is away
    }
}

void link_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_ERROR);    // "skip_common_name" is by design, every time
    presses = xQueueCreate(4, sizeof(press_t));
    task_in_psram(press_task, "press", 8192, 4, 1);
    task_in_psram(events_task, "events", 8192, 4, 1);   // core 1: core 0 is WiFi + the AFE's feed; no flash writes on it
}

bool link_connected(void) { return connected; }
