// link.c - see link.h.
#include "link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "light.h"
#include "net.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "link";
static bool connected;
static int last_refusal;
#define TEXT_WAIT_MS 20000             // a typed question is answered once a turn has started
#define PULL_WAIT_MS 15000
#define EVENTS_QUIET_MS 45000          // a comment line arrives every 20 s
#define EVENT_LINE 512
#define PULL_MAX 700                   // chars a pull asks for; UTF-8 and JSON make it ~3 KB at most
#define PULL_BUF 4096
#define REPLY_MAX 2400                 // what the screen draws of one reply (ui.c says the same)
#define TYPED_MAX 512

// --- what is being written ---------------------------------------------------------------
// The reply the board follows: its id, its revision, how much Sapphire has,
// how much the board has. `wanted` is set by a doorbell and cleared by a pull.

static SemaphoreHandle_t lock;
static char msg_id[24], msg_rev[12];
static int msg_have, msg_mine, msg_done;
static char *reply;                    // the text so far, REPLY_MAX + 1
static bool wanted;

typedef struct { char kind; char text[TYPED_MAX]; } job_t;   // 't' typed text, 'p' pull
static QueueHandle_t jobs;

static void url_for(const char *door, char *out, size_t size)
{
    snprintf(out, size, "%s/api/devices/%s/%s", settings.sapphire, settings.name, door);
}

static esp_http_client_handle_t open_client(const char *door, esp_http_client_method_t method, int timeout_ms)
{
    char url[SETTING_MAX + 160], bearer[SETTING_MAX + 8];
    url_for(door, url, sizeof url);
    snprintf(bearer, sizeof bearer, "Bearer %s", settings.voice_key);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .cert_pem = settings.cert[0] ? settings.cert : NULL,
        .skip_cert_common_name_check = true,      // her certificate names no address
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    if (!settings.cert[0]) cfg.crt_bundle_attach = NULL;   // no certificate given at setup: not checked, as documented
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) esp_http_client_set_header(c, "Authorization", bearer);
    return c;
}

// the whole answer of a small request, or -1. `body` gets at most size-1 bytes.
static int small_answer(esp_http_client_handle_t c, char *body, size_t size)
{
    if (esp_http_client_fetch_headers(c) < 0) return -1;
    int total = 0;
    while (total < (int)size - 1) {
        int n = esp_http_client_read(c, body + total, size - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    body[total] = 0;
    return esp_http_client_get_status_code(c);
}

// --- typed text up ------------------------------------------------------------------------

static void post_text(const char *text)
{
    esp_http_client_handle_t c = open_client("text", HTTP_METHOD_POST, TEXT_WAIT_MS);
    if (!c) { ui_failed("not sent: no memory"); return; }
    esp_http_client_set_header(c, "Content-Type", "text/plain; charset=utf-8");
    size_t len = strlen(text);
    bool ok = false;
    char why[96] = "not sent: Sapphire did not answer";
    if (esp_http_client_open(c, len) == ESP_OK && esp_http_client_write(c, text, len) == (int)len) {
        char body[512];
        int status = small_answer(c, body, sizeof body);
        cJSON *j = cJSON_Parse(body);
        if (status == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "ok"))) {
            ok = cJSON_IsTrue(cJSON_GetObjectItem(j, "accepted"));
            if (!ok) strlcpy(why, "not taken: she is busy", sizeof why);
        } else {
            const cJSON *e = j ? cJSON_GetObjectItem(j, "error") : NULL;
            const cJSON *d = j ? cJSON_GetObjectItem(j, "detail") : NULL;
            snprintf(why, sizeof why, "not sent: %s", cJSON_IsString(e) ? e->valuestring
                     : cJSON_IsString(d) ? d->valuestring : status == 401 ? "wrong key, or not added yet" : "no answer");
        }
        cJSON_Delete(j);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!ok) { ui_failed(why); light_cue("error"); }
}

bool link_send_text(const char *text)
{
    if (!jobs || !net_up()) return false;
    job_t job = {.kind = 't'};
    strlcpy(job.text, text, sizeof job.text);
    return xQueueSend(jobs, &job, 0) == pdTRUE;
}

// --- her words down -------------------------------------------------------------------------

static void pull(void)
{
    char id[24], rev[12];
    int from;
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(id, msg_id, sizeof id);
    strlcpy(rev, msg_rev, sizeof rev);
    from = msg_mine;
    wanted = false;
    xSemaphoreGive(lock);
    if (!id[0] || from >= REPLY_MAX) return;

    char door[96];
    snprintf(door, sizeof door, "text?msg=%s&rev=%s&from=%d&max=%d", id, rev, from, PULL_MAX);
    esp_http_client_handle_t c = open_client(door, HTTP_METHOD_GET, PULL_WAIT_MS);
    if (!c) return;
    char *body = malloc(PULL_BUF);
    if (!body) { esp_http_client_cleanup(c); return; }
    int status = esp_http_client_open(c, 0) == ESP_OK ? small_answer(c, body, PULL_BUF) : -1;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    cJSON *j = status == 200 ? cJSON_Parse(body) : NULL;
    free(body);
    if (!j) { ESP_LOGW(TAG, "pull of %s from %d answered %d", id, from, status); return; }
    const cJSON *got_id = cJSON_GetObjectItem(j, "msg");
    const cJSON *got_rev = cJSON_GetObjectItem(j, "rev");
    const cJSON *text = cJSON_GetObjectItem(j, "text");
    const cJSON *at = cJSON_GetObjectItem(j, "from");
    const cJSON *have = cJSON_GetObjectItem(j, "have");
    bool changed = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (cJSON_IsString(got_id) && !strcmp(got_id->valuestring, msg_id)) {
        int got = cJSON_IsString(got_rev) ? atoi(got_rev->valuestring) : 0, mine = atoi(msg_rev);
        if (got < mine) {
            // an older revision, answered after a newer doorbell: stale, dropped. The pull
            // for the new one is queued already (a rewind here made two bubbles, 2026-10-06)
        } else if (got > mine) {
            snprintf(msg_rev, sizeof msg_rev, "%d", got);   // newer than any doorbell said: from the start
            msg_mine = 0;
            reply[0] = 0;
            wanted = true;
        } else if (cJSON_IsString(text) && cJSON_IsNumber(at) && (int)at->valuedouble == msg_mine) {
            size_t room = REPLY_MAX - msg_mine;
            size_t n = strlen(text->valuestring);
            if (n > room) n = room;
            memcpy(reply + msg_mine, text->valuestring, n);
            msg_mine += n;
            reply[msg_mine] = 0;
            changed = n > 0;
            if (cJSON_IsNumber(have)) msg_have = (int)have->valuedouble;
            if (msg_mine < msg_have && n > 0) wanted = true;   // more than one pull's worth: again
        }
    }
    xSemaphoreGive(lock);
    cJSON_Delete(j);
    if (changed) ui_reply_set(reply);
}

static void doorbell(const cJSON *j)
{
    const cJSON *id = cJSON_GetObjectItem(j, "msg");
    const cJSON *rev = cJSON_GetObjectItem(j, "rev");
    const cJSON *have = cJSON_GetObjectItem(j, "have");
    const cJSON *done = cJSON_GetObjectItem(j, "done");
    if (!cJSON_IsString(id)) return;
    bool fresh = false, ring = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (strcmp(id->valuestring, msg_id)) {
        strlcpy(msg_id, id->valuestring, sizeof msg_id);
        strlcpy(msg_rev, cJSON_IsString(rev) ? rev->valuestring : "0", sizeof msg_rev);
        msg_mine = msg_have = msg_done = 0;
        reply[0] = 0;
        fresh = true;
    } else if (cJSON_IsString(rev) && strcmp(rev->valuestring, msg_rev)) {
        strlcpy(msg_rev, rev->valuestring, sizeof msg_rev);
        msg_mine = 0;
        reply[0] = 0;
    }
    if (cJSON_IsNumber(have)) msg_have = (int)have->valuedouble;
    if (cJSON_IsTrue(done)) msg_done = 1;
    if (msg_mine < msg_have && !wanted) { wanted = true; ring = true; }
    xSemaphoreGive(lock);
    if (fresh) ui_reply_begin();
    if (ring) { job_t job = {.kind = 'p'}; xQueueSend(jobs, &job, 0); }
}

static void take_line(const char *line)
{
    if (strncmp(line, "data: ", 6)) return;
    cJSON *j = cJSON_Parse(line + 6);
    const cJSON *state = j ? cJSON_GetObjectItem(j, "state") : NULL;
    if (cJSON_IsString(state)) {
        const char *s = state->valuestring;
        if (!strcmp(s, "connected")) {
            connected = true;
            last_refusal = 0;
            const cJSON *now = cJSON_GetObjectItem(j, "now");
            const cJSON *tz = cJSON_GetObjectItem(j, "tz");
            if (cJSON_IsString(tz) && tz->valuestring[0]) { setenv("TZ", tz->valuestring, 1); tzset(); }
            if (cJSON_IsNumber(now)) {
                struct timeval tv = {.tv_sec = (time_t)now->valuedouble};
                settimeofday(&tv, NULL);
            }
            ESP_LOGI(TAG, "events stream open%s", cJSON_IsNumber(now) ? ", clock set" : "");
            light_cue_from_brain("idle");
            ui_link(true);
            ui_status("");
        }
        else if (!strcmp(s, "text")) doorbell(j);
        else if (!strcmp(s, "thinking")) { light_cue_from_brain(s); ui_status("thinking"); }
        else if (!strcmp(s, "tool")) {
            const cJSON *name = cJSON_GetObjectItem(j, "tool_name");
            char text[48];
            snprintf(text, sizeof text, "%s", cJSON_IsString(name) && name->valuestring[0] ? name->valuestring : "a tool");
            light_cue_from_brain(s);
            ui_status(text);
        }
        else if (!strcmp(s, "idle") || !strcmp(s, "error")) {
            light_cue_from_brain(s);
            ui_status(!strcmp(s, "error") ? "no answer" : "");
            if (!strcmp(s, "idle")) {
                xSemaphoreTake(lock, portMAX_DELAY);
                bool behind = msg_id[0] && msg_mine < msg_have && !wanted;
                if (behind) wanted = true;
                xSemaphoreGive(lock);
                if (behind) { job_t job = {.kind = 'p'}; xQueueSend(jobs, &job, 0); }
            }
            job_t job = {.kind = 'd', .text = {0}};           // after any pull in the queue: close the bubble
            xQueueSend(jobs, &job, 0);
        }
    }
    cJSON_Delete(j);
}

static void hold_events(void)
{
    esp_http_client_handle_t c = open_client("events", HTTP_METHOD_GET, EVENTS_QUIET_MS);
    if (!c) return;
    esp_err_t opened = esp_http_client_open(c, 0);
    int64_t length = opened == ESP_OK ? esp_http_client_fetch_headers(c) : -1;
    if (opened == ESP_OK && length >= 0 && esp_http_client_get_status_code(c) == 200) {
        // One byte at a time: esp_http_client_read() only returns when the
        // buffer it was given is full or the stream ends. The stream is tiny.
        char line[EVENT_LINE];
        size_t have = 0;
        for (;;) {
            char ch;
            int n = esp_http_client_read(c, &ch, 1);
            if (n <= 0) { ESP_LOGW(TAG, "events stream ended (%d)", n); break; }
            if (ch == '\n') { line[have] = 0; take_line(line); have = 0; }
            else if (have < sizeof line - 1) line[have++] = ch;
        }
    } else {
        int status = esp_http_client_get_status_code(c);
        if (status != last_refusal) {
            last_refusal = status;
            ESP_LOGW(TAG, "events stream refused: %d%s", status,
                     status == 401 ? " (not added in Settings > Devices yet, or a wrong key)" : "");
            ui_status(status == 401 ? "not added yet" : "no link");
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

static void events_task(void *arg)
{
    int pause = 1;
    for (;;) {
        if (net_up()) {
            hold_events();
            connected = false;
            ui_link(false);
            light_cue("nolink");
        }
        vTaskDelay(pdMS_TO_TICKS(pause * 1000));
        pause = pause < 30 ? pause * 2 : 30;
        if (net_up() && pause > 8) pause = 8;
    }
}

static void client_task(void *arg)
{
    job_t job;
    for (;;) {
        if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        if (job.kind == 't') { light_cue("thinking"); ui_status("sending"); post_text(job.text); }
        else if (job.kind == 'p') {
            pull();
            bool again;
            xSemaphoreTake(lock, portMAX_DELAY);
            again = wanted;
            xSemaphoreGive(lock);
            if (again) xQueueSend(jobs, &job, 0);
        }
        else if (job.kind == 'd') {
            bool behind, pending;
            xSemaphoreTake(lock, portMAX_DELAY);
            pending = wanted;
            behind = msg_id[0] && msg_mine < msg_have && msg_mine < REPLY_MAX;
            if (behind) wanted = true;
            xSemaphoreGive(lock);
            if ((behind || pending) && job.text[0] < 6) {    // the last pieces first, then done again
                if (!pending) pull();
                job.text[0]++;
                xQueueSend(jobs, &job, 0);
            } else {
                ui_reply_done();
            }
        }
    }
}

void link_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    lock = xSemaphoreCreateMutex();
    reply = calloc(REPLY_MAX + 1, 1);
    jobs = xQueueCreate(6, sizeof(job_t));
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_ERROR);    // "skip_common_name" is by design, every time
    xTaskCreatePinnedToCore(events_task, "events", 6144, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(client_task, "client", 6144, NULL, 4, NULL, 1);
}

bool link_connected(void) { return connected; }
