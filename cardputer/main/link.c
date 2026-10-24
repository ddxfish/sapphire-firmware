// link.c - the Cardputer's side of Sapphire (P1: register + hold the events
// stream). Adapted from ai-speaker-mic/link.c, trimmed to the events door and
// with the task stack on INTERNAL RAM (this board has no PSRAM).
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
#include "freertos/task.h"

#include "board.h"
#include "light.h"
#include "net.h"
#include "settings.h"
#include "wav.h"

static const char *TAG = "link";
static bool connected;
static int last_refusal;                 // the events door's last status, said once
#define EVENTS_QUIET_MS 45000            // a comment line arrives every 20 s
#define EVENT_LINE 1024

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

// --- push a recording to Sapphire's voice door (P2): POST /voice, 16 kHz mono WAV ---
#define CLAIM_WAIT_MS 3000
#define VOICE_WAIT_MS 90000
#define VOICE_ANSWER 8192

bool link_send_voice(const int16_t *pcm, size_t samples, char *heard, size_t heard_size)
{
    if (heard && heard_size) heard[0] = 0;
    if (!net_up()) { ESP_LOGW(TAG, "no WiFi: the recording is lost"); return false; }
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
            char *body = malloc(VOICE_ANSWER);        // internal RAM: this board has no PSRAM
            int n = body ? esp_http_client_read_response(c, body, VOICE_ANSWER - 1) : 0;
            if (n > 0) body[n] = 0;
            cJSON *j = (body && n > 0) ? cJSON_Parse(body) : NULL;
            cJSON *h = j ? cJSON_GetObjectItem(j, "heard") : NULL;
            cJSON *e = j ? cJSON_GetObjectItem(j, "error") : NULL;
            if (heard && cJSON_IsString(h)) strlcpy(heard, h->valuestring, heard_size);
            ok = status == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "ok"));
            if (!ok) ESP_LOGW(TAG, "voice door answered %d: %.200s", status,
                              cJSON_IsString(e) ? e->valuestring : (body ? body : ""));
            cJSON_Delete(j);
            free(body);
        } else {
            ESP_LOGW(TAG, "the recording did not reach Sapphire");
        }
    } else {
        ESP_LOGW(TAG, "could not reach Sapphire at %s", settings.sapphire);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

// --- streaming voice (P2 push-to-talk): a chunked POST we feed as we record ---
// esp_http_client sets the chunked header but does not frame chunks, so we do:
// each piece goes out as "<hex len>\r\n" + bytes + "\r\n", ended with "0\r\n\r\n".
static esp_http_client_handle_t vclient;    // the open voice stream (PTT is serial, one at a time)
static bool vok;

static bool chunk_write(esp_http_client_handle_t c, const void *data, size_t len)
{
    char hdr[12];
    int hn = snprintf(hdr, sizeof hdr, "%x\r\n", (unsigned)len);
    if (esp_http_client_write(c, hdr, hn) != hn) return false;
    if (len && esp_http_client_write(c, data, len) != (int)len) return false;
    return esp_http_client_write(c, "\r\n", 2) == 2;
}

bool link_voice_begin(void)
{
    if (vclient) return false;
    if (!net_up()) { ESP_LOGW(TAG, "no WiFi: cannot start a recording"); return false; }
    esp_http_client_handle_t c = open_client("voice", HTTP_METHOD_POST, VOICE_WAIT_MS);
    if (!c) return false;
    esp_http_client_set_header(c, "Content-Type", "audio/wav");
    if (esp_http_client_open(c, -1) != ESP_OK) {        // -1 -> Transfer-Encoding: chunked
        esp_http_client_cleanup(c);
        ESP_LOGW(TAG, "could not reach Sapphire at %s", settings.sapphire);
        return false;
    }
    uint8_t head[WAV_HEADER];
    wav_header_stream(head, AUDIO_RATE, 1);             // sentinel length; she reads to the end
    vclient = c;
    vok = chunk_write(c, head, WAV_HEADER);
    return vok;
}

bool link_voice_write(const int16_t *pcm, size_t samples)
{
    if (!vclient || !vok) return false;
    vok = chunk_write(vclient, pcm, samples * 2);
    return vok;
}

bool link_voice_end(char *heard, size_t heard_size)
{
    if (heard && heard_size) heard[0] = 0;
    if (!vclient) return false;
    esp_http_client_handle_t c = vclient;
    vclient = NULL;
    bool ok = false;
    if (vok && esp_http_client_write(c, "0\r\n\r\n", 5) == 5 && esp_http_client_fetch_headers(c) >= 0) {
        int status = esp_http_client_get_status_code(c);
        char *body = malloc(VOICE_ANSWER);              // internal RAM: this board has no PSRAM
        int n = body ? esp_http_client_read_response(c, body, VOICE_ANSWER - 1) : 0;
        if (n > 0) body[n] = 0;
        cJSON *j = (body && n > 0) ? cJSON_Parse(body) : NULL;
        cJSON *h = j ? cJSON_GetObjectItem(j, "heard") : NULL;
        cJSON *e = j ? cJSON_GetObjectItem(j, "error") : NULL;
        if (heard && cJSON_IsString(h)) strlcpy(heard, h->valuestring, heard_size);
        ok = status == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "ok"));
        if (!ok) ESP_LOGW(TAG, "voice door answered %d: %.200s", status,
                          cJSON_IsString(e) ? e->valuestring : (body ? body : ""));
        cJSON_Delete(j);
        free(body);
    } else {
        ESP_LOGW(TAG, "the recording did not reach Sapphire");
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

static void take_line(const char *line)
{
    if (strncmp(line, "data: ", 6)) return;                 // ": still here" and blank lines
    cJSON *j = cJSON_Parse(line + 6);
    cJSON *state = j ? cJSON_GetObjectItem(j, "state") : NULL;
    if (cJSON_IsString(state)) {
        if (!strcmp(state->valuestring, "connected")) {
            connected = true;
            last_refusal = 0;
            // Sapphire's clock and time zone ride along, so the board keeps
            // local time without an internet clock.
            const cJSON *now = cJSON_GetObjectItem(j, "now");
            const cJSON *tz = cJSON_GetObjectItem(j, "tz");
            if (cJSON_IsString(tz) && tz->valuestring[0]) { setenv("TZ", tz->valuestring, 1); tzset(); }
            if (cJSON_IsNumber(now)) {
                struct timeval tv = {.tv_sec = (time_t)now->valuedouble};
                settimeofday(&tv, NULL);
            }
            ESP_LOGI(TAG, "events stream open%s", cJSON_IsNumber(now) ? ", clock set" : "");
            light_cue("idle");
        } else {
            // the brain's state drives the walkie status word + corner LED. shell_cue
            // paints it only while the Walkie app is open, so the launcher is left alone.
            const char *s = state->valuestring, *cue = NULL;
            if (!strcmp(s, "listening")) cue = "listen";
            else if (!strcmp(s, "thinking") || !strcmp(s, "tool")) cue = "think";
            else if (!strcmp(s, "speaking")) cue = "speak";
            else if (!strcmp(s, "idle")) cue = "idle";
            else if (!strcmp(s, "error")) cue = "nolink";
            ESP_LOGI(TAG, "state: %s", s);
            if (cue) light_cue(cue);
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
        // One byte at a time: the stream sends a few bytes every 20 s and
        // esp_http_client_read only returns on a full buffer or the end.
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
        if (status != last_refusal) {              // said once, not every few seconds
            last_refusal = status;
            ESP_LOGW(TAG, "events stream refused: %d%s", status,
                     status == 401 ? " (not added in Settings > Devices yet, or a wrong key)" : "");
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
            if (connected) pause = 1;                      // it had been open: back at once
            connected = false;
            light_cue("nolink");
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
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_ERROR);    // "skip_common_name" is by design
    xTaskCreatePinnedToCore(events_task, "events", 8192, NULL, 4, NULL, 1);   // internal RAM: no PSRAM here
}

bool link_connected(void) { return connected; }
