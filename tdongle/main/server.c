// server.c - see server.h.
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
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

#include "board.h"
#include "buttons.h"
#include "light.h"
#include "link.h"
#include "net.h"
#include "ota.h"
#include "settings.h"
#include "status.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "server";
static httpd_handle_t server;
#define JSON_MAX 4096
static QueueHandle_t store_queue;      // /storage PUT, GET and format: a minute each
#define STORE_CHUNK 4096
#define STORE_PACE_MS 4                // after each 4 KB of a backup: ~1 MB/s, so WiFi and the screen keep up
static const uint8_t BAK_MAGIC[12] = "SAPPHIREBAK\x01";

static esp_err_t say_json(httpd_req_t *req, int status, cJSON *json)
{
    httpd_resp_set_status(req, status == 200 ? "200 OK" : status == 400 ? "400 Bad Request"
                          : status == 401 ? "401 Unauthorized" : status == 404 ? "404 Not Found"
                          : status == 413 ? "413 Payload Too Large" : "500 Internal Server Error");
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

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len > JSON_MAX) { refuse(req, 413, "too much"); return NULL; }
    char *body = malloc(req->content_len + 1);
    if (!body) { refuse(req, 500, "no memory"); return NULL; }
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

// The card for /health and GET /storage: room, what it can do, and what the
// check found. `checks` only on a board that keeps hashes.
static cJSON *storage_json(const storage_state_t *card)
{
    cJSON *st = cJSON_CreateObject();
    cJSON_AddBoolToObject(st, "mounted", card->mounted);
    if (!card->mounted) return st;
    uint64_t free_b = 0, total = 0;
    if (storage_room(&free_b, &total)) {
        cJSON_AddNumberToObject(st, "free_bytes", (double)free_b);
        cJSON_AddNumberToObject(st, "total_bytes", (double)total);
    }
    cJSON *can = cJSON_AddArrayToObject(st, "can");
    cJSON_AddItemToArray(can, cJSON_CreateString("format"));
#ifdef BOARD_STORAGE_CHECKS
    cJSON_AddItemToArray(can, cJSON_CreateString("check"));
    cJSON *ch = cJSON_AddObjectToObject(st, "checks");
    cJSON_AddBoolToObject(ch, "running", card->checking);
    cJSON_AddNumberToObject(ch, "done", card->checked);
    cJSON_AddNumberToObject(ch, "of", card->to_check);
    cJSON_AddNumberToObject(ch, "damaged", card->damaged);
    cJSON_AddNumberToObject(ch, "unchecked", card->unchecked);
#endif
    cJSON_AddNumberToObject(st, "count", card->count);
    cJSON_AddBoolToObject(st, "writing", card->writing);
    cJSON_AddBoolToObject(st, "failed", card->failed);
    return st;
}

// --- /health ---------------------------------------------------------------------------

static esp_err_t on_health(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "name", settings.name);
    cJSON_AddStringToObject(j, "board", BOARD_NAME);
    cJSON_AddStringToObject(j, "firmware", PROJECT_VER);
    cJSON_AddStringToObject(j, "mac", board_mac());             // the chip's own id: the flasher knows a board by it
    cJSON_AddStringToObject(j, "model", FIRMWARE_MODEL);         // which program this is: the firmware index's board id
    cJSON_AddStringToObject(j, "slot", ota_slot());
    cJSON *has = cJSON_AddArrayToObject(j, "has");
    cJSON_AddItemToArray(has, cJSON_CreateString("screen"));
    cJSON_AddItemToArray(has, cJSON_CreateString("light"));
    cJSON_AddItemToArray(has, cJSON_CreateString("power"));
    cJSON_AddItemToArray(has, cJSON_CreateString("buttons"));   // BOOT: what each press is set to comes on the events stream
    cJSON_AddItemToObject(j, "buttons", buttons_json());
    if (ota_can()) cJSON_AddItemToArray(has, cJSON_CreateString("firmware"));   // two slots: PUT /firmware
    storage_state_t card;
    status_t st = status_now(&card);
    cJSON_AddStringToObject(j, "status", status_word(st));   // the stick's one word: what its LED and screen say
    if (card.mounted) cJSON_AddItemToArray(has, cJSON_CreateString("storage"));
    cJSON_AddItemToObject(j, "storage", storage_json(&card));
    cJSON *screen = cJSON_AddObjectToObject(j, "screen");    // what its screen takes: text, and this one setting. No picture
    cJSON_AddBoolToObject(screen, "flip", board()->flip);
    cJSON_AddItemToObject(j, "led", light_status());
    cJSON_AddNumberToObject(j, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON *link = cJSON_AddObjectToObject(j, "link");
    cJSON_AddBoolToObject(link, "connected", link_connected());
    cJSON_AddNumberToObject(j, "wifi_dbm", net_rssi());
    cJSON_AddNumberToObject(j, "brightness", settings.brightness);
    cJSON_AddNumberToObject(j, "free_kb", esp_get_free_heap_size() / 1024);
    cJSON_AddNumberToObject(j, "free_internal_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(j, "free_internal_largest_kb", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024);
    return say_json(req, 200, j);
}

// --- /led -------------------------------------------------------------------------------

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
            return refuse(req, 400, "a color or animation this light does not know; GET /led/spec lists them");
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
        if (!ok) return refuse(req, 400, "a color or animation this light does not know; GET /led/spec lists them");
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
    if (!cJSON_IsString(v) || sscanf(v->valuestring, "%d:%d", &h, &m) != 2) return fallback;
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
            snprintf(why, sizeof why, "the %s look names a color or animation this light does not know", LIGHT_SLOTS[i]);
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

// --- /screen: a line of hers over the two lines ----------------------------------------------------------

static esp_err_t on_screen(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *in = read_json(req);
    if (!in) return ESP_OK;
    const cJSON *text = cJSON_GetObjectItem(in, "text");
    const cJSON *secs = cJSON_GetObjectItem(in, "seconds");
    if (cJSON_IsTrue(cJSON_GetObjectItem(in, "clear"))) {
        ui_toast("", 1);
        cJSON_Delete(in);
        cJSON *out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        return say_json(req, 200, out);
    }
    if (!cJSON_IsString(text) || !text->valuestring[0]) { cJSON_Delete(in); return refuse(req, 400, "text is needed"); }
    int seconds = cJSON_IsNumber(secs) ? (int)secs->valuedouble : 20;
    ui_toast(text->valuestring, seconds);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "seconds", seconds);
    cJSON_AddNumberToObject(out, "shown_chars", strlen(text->valuestring) > 300 ? 300 : strlen(text->valuestring));
    cJSON_Delete(in);
    return say_json(req, 200, out);
}

// --- /power ---------------------------------------------------------------------------------

static void restart_soon(void *arg) { vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }

static void sleep_soon(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    board_backlight(0);
    board_led(0, 0, 0);
    esp_sleep_enable_ext0_wakeup(BOARD_BOOT_BUTTON, 0);   // BOOT brings it back
    esp_deep_sleep_start();
}

// PUT /screen/settings {"flip": true|false}: the picture turned 180 degrees, kept. The board
// restarts to draw that way; a card being written is left to finish first.
static esp_err_t on_screen_settings(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *in = read_json(req);
    if (!in) return ESP_OK;
    const cJSON *flip = cJSON_GetObjectItem(in, "flip");
    bool want = cJSON_IsTrue(flip);
    bool turn = cJSON_IsBool(flip) && want != board()->flip;
    cJSON_Delete(in);
    if (turn && storage_busy()) return refuse(req, 409, "the card is being written: save again when it is done");
    if (turn) {
        board_set_flip(want);
        xTaskCreate(restart_soon, "restart", 2048, NULL, 5, NULL);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "flip", board()->flip);
    cJSON_AddBoolToObject(j, "restarting", turn);
    return say_json(req, 200, j);
}

static esp_err_t on_power(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    char action[16];
    if (!query_value(req, "action", action, sizeof action)) return refuse(req, 400, "action=restart or shutdown");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddNumberToObject(j, "in_s", 2);
    if (!strcmp(action, "restart")) xTaskCreate(restart_soon, "restart", 2048, NULL, 5, NULL);
    else if (!strcmp(action, "shutdown")) xTaskCreate(sleep_soon, "sleep", 2048, NULL, 5, NULL);
    else { cJSON_Delete(j); return refuse(req, 400, "action=restart or shutdown"); }
    return say_json(req, 200, j);
}

static const char *file_name(httpd_req_t *req) { return req->uri + strlen("/storage/"); }

static esp_err_t on_storage(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!storage_ok()) return refuse(req, 503, "no card");
    storage_state_t card;
    storage_state(&card);
    cJSON *j = storage_json(&card);
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "path", storage_dir());
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
    uint8_t *buf = malloc(STORE_CHUNK);
    size_t left = req->content_len, seen = 0;
    uint8_t head[12];
    int problem = buf ? 0 : 507;
    while (left > 0 && !problem) {
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
    if (problem) {
        storage_write_drop(w);
        if (problem == 422) refuse(req, 400, "not a sealed backup: plaintext never lands on this card");
        else if (problem == 507) refuse(req, 507, "the card is full or failed while writing");
        else refuse(req, 400, "the backup did not arrive whole");
        goto done;
    }
    char sha[65];
    uint64_t size = 0;
    if (!storage_write_finish(w, sha, &size)) { refuse(req, 507, "the card could not keep the backup"); goto done; }
    char want[72] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Sha256", want, sizeof want) == ESP_OK && want[0] && strcasecmp(want, sha)) {
        storage_remove(name);
        refuse(req, 422, "sha256 mismatch: dropped");
        goto done;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "name", name);
    cJSON_AddNumberToObject(j, "size", (double)size);
    cJSON_AddStringToObject(j, "sha256", sha);
    say_json(req, 200, j);
done:
    httpd_req_async_handler_complete(req);
}

static void store_get(httpd_req_t *req)
{
    char path[200];
    FILE *f = storage_path(file_name(req), path, sizeof path) ? fopen(path, "rb") : NULL;
    uint8_t *buf = f ? malloc(STORE_CHUNK) : NULL;
    if (!buf) { if (f) fclose(f); refuse(req, f ? 500 : 404, f ? "no memory" : "no such file"); httpd_req_async_handler_complete(req); return; }
    httpd_resp_set_type(req, "application/octet-stream");
    size_t n;
    bool ok = true;
    while (ok && (n = fread(buf, 1, STORE_CHUNK, f)) > 0) ok = httpd_resp_send_chunk(req, (char *)buf, n) == ESP_OK;
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

// PUT /firmware: her program over the air, written by ota.c on the store
// task (one body at a time there: a backup and a program never race).
static void firmware_put(httpd_req_t *req) { ota_put(req, STORE_PACE_MS, NULL); }

static esp_err_t on_firmware(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    return hand_to_store(req, firmware_put);
}

static esp_err_t on_storage_file(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!storage_ok()) return refuse(req, 503, "no card");
    const char *name = file_name(req);
    if (!storage_name_ok(name)) return refuse(req, 400, "bad name");
    if (req->method == HTTP_DELETE) {
        if (!storage_remove(name)) return refuse(req, 404, "no such file, or the card is busy");
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        return say_json(req, 200, j);
    }
    if (req->method == HTTP_GET) return hand_to_store(req, store_get);
    if (!req->content_len) return refuse(req, 411, "Content-Length required");
    size_t n = strlen(name);
    bool sealed_name = n > 12 && !strcmp(name + n - 12, ".sapphirebak");
    if (!sealed_name && !storage_name_plain_ok(name)) return refuse(req, 400, "only sealed backups (.sapphirebak) land here");
    uint64_t free_b = 0;
    if (storage_room(&free_b, NULL) && (uint64_t)req->content_len + (16u << 20) > free_b)
        return refuse(req, 507, "not enough room on the card");
    if (storage_busy()) return refuse(req, 409, "another backup is arriving");
    return hand_to_store(req, store_put);
}

static esp_err_t on_storage_format(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!storage_ok()) return refuse(req, 503, "no card");
    if (storage_busy()) return refuse(req, 409, "a backup is arriving");
    return hand_to_store(req, store_format);
}

// POST /storage/check: every backup read back against the hash kept when it was written, now.
// It runs by itself; /health `storage.checks` says how far it is and what it found.
static esp_err_t on_storage_check(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!storage_ok()) return refuse(req, 503, "no card");
    if (!storage_check_now()) return refuse(req, 409, "a check is running already");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "checking", true);
    return say_json(req, 200, j);
}

static esp_err_t on_missing(httpd_req_t *req, httpd_err_code_t err)
{
    return refuse(req, 404, "this board has no such address");
}

void server_start(void)
{
    if (server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 24;
    cfg.max_open_sockets = 4;
    cfg.core_id = 1;
    cfg.uri_match_fn = httpd_uri_match_wildcard;        // /storage/* ; every other route is still exact
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) != ESP_OK) { ESP_LOGE(TAG, "the server did not start"); return; }
    store_queue = xQueueCreate(1, sizeof(store_job_t));
    xTaskCreatePinnedToCore(store_task, "store", 6144, NULL, 3, NULL, 1);
    const httpd_uri_t routes[] = {              // exact routes first; the /storage/* wildcard last
        {"/health", HTTP_GET, on_health, NULL},
        {"/led", HTTP_POST, on_led, NULL},
        {"/led/spec", HTTP_GET, on_led_spec, NULL},
        {"/led/baseline", HTTP_GET, on_led_baseline, NULL},
        {"/led/baseline", HTTP_PUT, on_led_baseline, NULL},
        {"/led/looks", HTTP_PUT, on_led_looks, NULL},
        {"/screen", HTTP_POST, on_screen, NULL},
        {"/screen/settings", HTTP_PUT, on_screen_settings, NULL},
        {"/power", HTTP_POST, on_power, NULL},
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
