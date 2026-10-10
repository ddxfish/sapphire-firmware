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
#include "freertos/task.h"

#include "buttons.h"
#include "net.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "link";
static bool connected;
static int last_refusal;
#define PRESS_WAIT_MS 20000
#define EVENTS_QUIET_MS 45000          // a comment line arrives every 20 s
#define EVENT_LINE 512

typedef struct { char button[24], how[8], job[16]; } job_t;
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

// --- a press up ------------------------------------------------------------------------------

static void post_press(const job_t *p)
{
    char out[112];
    int len = snprintf(out, sizeof out, "{\"button\": \"%s\", \"how\": \"%s\"%s%s%s}", p->button, p->how,
                       p->job[0] ? ", \"job\": \"" : "", p->job, p->job[0] ? "\"" : "");
    esp_http_client_handle_t c = open_client("press", HTTP_METHOD_POST, PRESS_WAIT_MS);
    if (!c) { ui_toast("Not sent: no memory", 8); return; }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    char why[96] = "Sapphire did not answer";
    bool ok = false;
    if (esp_http_client_open(c, len) == ESP_OK && esp_http_client_write(c, out, len) == len) {
        char *body = malloc(1024);
        int status = body ? small_answer(c, body, 1024) : -1;
        cJSON *j = status > 0 && body ? cJSON_Parse(body) : NULL;
        const cJSON *said = j ? cJSON_GetObjectItem(j, "said") : NULL;
        const cJSON *e = j ? cJSON_GetObjectItem(j, "error") : NULL;
        if (!e && j) e = cJSON_GetObjectItem(j, "detail");
        ok = status == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "ok"));
        if (ok) strlcpy(why, cJSON_IsString(said) ? said->valuestring
                        : cJSON_IsTrue(cJSON_GetObjectItem(j, "accepted")) ? "Sent to Sapphire" : "Nothing is set for that press", sizeof why);
        else if (cJSON_IsString(e)) strlcpy(why, e->valuestring, sizeof why);
        else if (status > 0) snprintf(why, sizeof why, "Sapphire answered %d", status);
        cJSON_Delete(j);
        free(body);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "%s %s%s%s: %s", p->button, p->how, p->job[0] ? " " : "", p->job, why);
    ui_toast(why, ok ? 6 : 10);
}

static void queue_press(const char *button, const char *how, const char *job)
{
    ui_wake();
    if (!jobs || !connected) { ui_toast("No link to Sapphire", 6); return; }
    job_t p = {0};
    strlcpy(p.button, button, sizeof p.button);
    strlcpy(p.how, how, sizeof p.how);
    strlcpy(p.job, job ? job : "", sizeof p.job);
    if (xQueueSend(jobs, &p, 0) != pdTRUE) ui_toast("Still on the last press", 4);
}

void link_press(const char *button, const char *how) { queue_press(button, how, NULL); }
void link_ask(const char *job) { queue_press("boot", "short", job); }

// --- her stream down ------------------------------------------------------------------------

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
        }
        else if (!strcmp(s, "buttons")) buttons_bound(cJSON_GetObjectItem(j, "bound"));
        // thinking, tool, idle, text: her turns are not shown on a backup stick
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
            if (status == 401) ui_toast("Sapphire does not know this stick yet: add it in Settings > Devices", 30);
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
        }
        vTaskDelay(pdMS_TO_TICKS(pause * 1000));
        pause = pause < 30 ? pause * 2 : 30;
        if (net_up() && pause > 8) pause = 8;
    }
}

static void client_task(void *arg)
{
    job_t job;
    for (;;) if (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) post_press(&job);
}

void link_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    jobs = xQueueCreate(3, sizeof(job_t));
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_ERROR);    // "skip_common_name" is by design, every time
    xTaskCreatePinnedToCore(events_task, "events", 6144, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(client_task, "client", 6144, NULL, 4, NULL, 1);
}

bool link_connected(void) { return connected; }
