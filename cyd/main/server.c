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
#include "esp_lvgl_port.h"
#include "buttons.h"
#include "light.h"
#include "link.h"
#include "net.h"
#include "ota.h"
#include "settings.h"
#include "speaker.h"
#include "storage.h"
#include "ui.h"
#include "wav.h"

static const char *TAG = "server";
static httpd_handle_t server;
#define JSON_MAX 4096
// a picture goes to the panel in strips of BOARD_PICTURE_PIXELS (board.h): 320 x 12 x 2 = 7.5 KB, two of them
#define PICTURE_DEFAULT_S 60
#define PICTURE_MAX_S 3600
static QueueHandle_t picture_queue;    // /screen/picture runs on its own task: the server stays free
static QueueHandle_t speak_queue;      // /audio/speak too: a sentence plays for seconds
static QueueHandle_t store_queue;      // /storage PUT, GET and format: a minute each
#define SPEAK_CHUNK 4096
#define HALT_MS 3000                   // after a stop, the rest of that reply is refused this long
#define STORE_CHUNK 4096
#define STORE_PACE_MS 4                // after each 4 KB of a backup: ~1 MB/s, so WiFi and the screen keep up
static const uint8_t BAK_MAGIC[12] = "SAPPHIREBAK\x01";
static volatile bool speaking, cut;
static int64_t halted_until_ms;

static const char *status_line(int status)
{
    switch (status) {
    case 200: return "200 OK";                     case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";           case 404: return "404 Not Found";
    case 409: return "409 Conflict";               case 411: return "411 Length Required";
    case 413: return "413 Payload Too Large";      case 415: return "415 Unsupported Media Type";
    case 422: return "422 Unprocessable Entity";   case 503: return "503 Service Unavailable";
    case 507: return "507 Insufficient Storage";   default: return "500 Internal Server Error";
    }
}

static esp_err_t say_json(httpd_req_t *req, int status, cJSON *json)
{
    httpd_resp_set_status(req, status_line(status));
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

// A refusal of a body that was not read. httpd drains an unread body before
// the next request, on its one task: a refused 100 MB backup kept /health deaf
// for minutes (2026-10-16). A big one is cut off with the socket instead.
#define UNREAD_BIG 65536
static esp_err_t refuse_unread(httpd_req_t *req, int status, const char *detail)
{
    bool big = req->content_len > UNREAD_BIG;
    if (big) httpd_resp_set_hdr(req, "Connection", "close");
    refuse(req, status, detail);
    return big ? ESP_FAIL : ESP_OK;                 // ESP_FAIL: the server closes the socket, nothing is drained
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
        if (req->content_len > UNREAD_BIG) httpd_resp_set_hdr(req, "Connection", "close");   // the caller returns ESP_FAIL: see refuse_unread
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

// --- /sensors: what the board measures, now. A name ends in its unit (temp_c); the light
// sensor is the raw ADC reading, 0..4095, until it is calibrated against a real meter ----

static cJSON *sensors_json(void)
{
    cJSON *sensors = cJSON_CreateObject();
    cJSON_AddNumberToObject(sensors, "light", board_light_raw());
    return sensors;
}

static esp_err_t on_sensors(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "sensors", sensors_json());
    return say_json(req, 200, j);
}

// --- /health ---------------------------------------------------------------------------

static esp_err_t on_health(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    ota_fine();                                               // Sapphire found the board and asked: a program on trial is kept
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddStringToObject(j, "name", settings.name);
    cJSON_AddStringToObject(j, "board", BOARD_NAME);
    cJSON_AddStringToObject(j, "firmware", PROJECT_VER);
    cJSON_AddStringToObject(j, "mac", board_mac());             // the chip's own id: the flasher knows a board by it
    cJSON_AddStringToObject(j, "model", FIRMWARE_MODEL);         // which program this is: the firmware index's board id
    cJSON_AddStringToObject(j, "slot", ota_slot());
    cJSON *has = cJSON_AddArrayToObject(j, "has");
    cJSON_AddItemToArray(has, cJSON_CreateString("keyboard"));
    cJSON_AddItemToArray(has, cJSON_CreateString("screen"));
    cJSON_AddItemToArray(has, cJSON_CreateString("light"));
    cJSON_AddItemToArray(has, cJSON_CreateString("power"));
    if (speaker_ok()) cJSON_AddItemToArray(has, cJSON_CreateString("speaker"));
    bool card = storage_alive();                              // asks the card: one pulled and put back is mounted again
    if (card) cJSON_AddItemToArray(has, cJSON_CreateString("storage"));
    cJSON_AddItemToArray(has, cJSON_CreateString("sensors"));
    cJSON_AddItemToArray(has, cJSON_CreateString("buttons"));   // BOOT (0.4.0): what each press is set to comes on the events stream
    cJSON_AddItemToObject(j, "buttons", buttons_json());
    if (ota_can()) cJSON_AddItemToArray(has, cJSON_CreateString("firmware"));   // two slots: PUT /firmware
    if (speaker_ok()) {
        cJSON *plays = cJSON_AddObjectToObject(j, "plays");
        cJSON_AddStringToObject(plays, "type", "audio/wav");
        cJSON_AddNumberToObject(plays, "rate", SPEAKER_RATE);
        cJSON_AddNumberToObject(plays, "channels", 1);
        cJSON_AddNumberToObject(j, "volume", speaker_volume());
    }
    cJSON *st = cJSON_AddObjectToObject(j, "storage");       // always: the slot exists; mounted says if a card is in
    cJSON_AddBoolToObject(st, "mounted", card);
    if (card) {
        uint64_t free_b = 0, total = 0;
        if (storage_room(&free_b, &total)) {
            cJSON_AddNumberToObject(st, "free_bytes", (double)free_b);
            cJSON_AddNumberToObject(st, "total_bytes", (double)total);
        }
        cJSON *can = cJSON_AddArrayToObject(st, "can");
        cJSON_AddItemToArray(can, cJSON_CreateString("format"));
        cJSON_AddItemToArray(can, cJSON_CreateString("cancel"));           // POST /storage/cancel
    }
    storage_state_t kept;                                     // what Sapphire reads back: N on the card, one arriving, the last one failed
    storage_state(&kept);
    cJSON_AddNumberToObject(st, "count", kept.count);
    cJSON_AddBoolToObject(st, "writing", kept.writing);
    cJSON_AddBoolToObject(st, "failed", kept.failed);
    cJSON_AddItemToObject(j, "sensors", sensors_json());      // name: number; Sapphire shows each as a reading
    cJSON *screen = cJSON_AddObjectToObject(j, "screen");    // what /screen/picture takes
    cJSON_AddNumberToObject(screen, "w", board_w());            // as it sits: rotation turns these
    cJSON_AddNumberToObject(screen, "h", board_h());
    cJSON_AddStringToObject(screen, "format", "rgb565be");
    cJSON_AddNumberToObject(screen, "rotation", board()->rotation);   // 0, 90, 180, 270: PUT /screen/settings {"rotation"} turns the glass
    cJSON_AddBoolToObject(screen, "invert", board()->invert);        // the glass variants (the test card says): PUT /screen/settings {"invert", "mirror"}
    cJSON_AddBoolToObject(screen, "mirror", board()->mirror);
    if (board_touch_needs_cal()) cJSON_AddBoolToObject(screen, "calibrate", true);   // POST /screen/calibrate: a resistive glass
    cJSON_AddStringToObject(screen, "chat", ui_chat_name());    // the chat it shows; Sapphire sends it with POST /screen {"chat"}
    cJSON_AddStringToObject(screen, "brain", ui_brain_name());  // and the model: stated, so a differing one is sent too (the picker's list carries no model)
    cJSON_AddStringToObject(screen, "home", ui_home_name());    // the page it opens on; PUT /screen/settings {"home"} sets it
    cJSON_AddNumberToObject(screen, "brightness", settings.brightness);
    cJSON_AddItemToObject(j, "led", light_status());
    cJSON_AddNumberToObject(j, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON *link = cJSON_AddObjectToObject(j, "link");
    cJSON_AddBoolToObject(link, "connected", link_connected());
    cJSON_AddNumberToObject(j, "wifi_dbm", net_rssi());
    cJSON_AddNumberToObject(j, "light_raw", board_light_raw());
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

// --- /screen/picture: the whole glass, straight from the wire to the panel -----------------
// The body is RGB565, big-endian, w x h pixels (query w, h <= the screen),
// centred on black. No frame buffer on this chip: each strip of lines is
// painted as it arrives. LVGL is held still until a tap, the BOOT button or
// `seconds` run out, then it repaints its own screen.

static void picture_work(httpd_req_t *req)
{
    int W = board_w(), H = board_h(), LINES = BOARD_PICTURE_PIXELS / W;
    char text[16];
    int w = query_value(req, "w", text, sizeof text) ? atoi(text) : 0;
    int h = query_value(req, "h", text, sizeof text) ? atoi(text) : 0;
    int seconds = query_value(req, "seconds", text, sizeof text) ? atoi(text) : PICTURE_DEFAULT_S;
    if (seconds < 1) seconds = 1;
    if (seconds > PICTURE_MAX_S) seconds = PICTURE_MAX_S;
    if (w < 1 || h < 1 || w > W || h > H || req->content_len != (size_t)w * h * 2) {
        refuse(req, 400, "w and h up to the screen's, and exactly w*h*2 bytes of RGB565");
        httpd_req_async_handler_complete(req);
        return;
    }
    if (!ui_picture_begin()) {
        refuse(req, 409, "the screen is busy");
        httpd_req_async_handler_complete(req);
        return;
    }
    size_t cap = (size_t)W * LINES * 2;
    uint8_t *bufs[2] = {heap_caps_malloc(cap, MALLOC_CAP_DMA), heap_caps_malloc(cap, MALLOC_CAP_DMA)};
    esp_lcd_panel_handle_t panel = board()->panel;
    if (!bufs[0] || !bufs[1]) {
        free(bufs[0]); free(bufs[1]);
        ui_picture_end();
        refuse(req, 500, "no memory for a picture");
        httpd_req_async_handler_complete(req);
        return;
    }
    // black all over, in strips
    memset(bufs[0], 0, cap);
    for (int y = 0; y < H; y += LINES) {
        int lines = H - y < LINES ? H - y : LINES;
        esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + lines, bufs[0]);
        vTaskDelay(1);
    }
    // the picture, strip by strip as it arrives, buffers alternating so the
    // DMA of one strip is long done before that buffer is filled again
    int x0 = (W - w) / 2, y0 = (H - h) / 2;
    size_t row = (size_t)w * 2, strip = row * LINES, left = req->content_len, have = 0;
    int y = 0, which = 1, problem = 0;
    uint8_t *buf = bufs[which];
    while (left > 0) {
        size_t want = strip - have;
        if (want > left) want = left;
        int n = httpd_req_recv(req, (char *)buf + have, want);
        if (n <= 0) { problem = 400; break; }
        have += n;
        left -= n;
        if (have == strip || left == 0) {
            int lines = have / row;
            esp_lcd_panel_draw_bitmap(panel, x0, y0 + y, x0 + w, y0 + y + lines, buf);
            y += lines;
            which ^= 1;
            buf = bufs[which];
            have = 0;
        }
    }
    if (problem) refuse(req, 400, "the picture did not arrive whole");
    else {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        cJSON_AddNumberToObject(j, "seconds", seconds);
        say_json(req, 200, j);
    }
    httpd_req_async_handler_complete(req);
    // hold it: a tap, the button, or the time. LVGL is held, so the chip is ours to read
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    vTaskDelay(pdMS_TO_TICKS(400));                       // the tap that sent it is not the tap that ends it
    while (!problem && esp_timer_get_time() < end) {
        if (board_button() || board_touch_raw(NULL, NULL, NULL)) break;
        if (uxQueueMessagesWaiting(picture_queue)) break;  // a newer picture waits: this one goes
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(1);                                         // the last strip's DMA
    free(bufs[0]); free(bufs[1]);
    // the finger or the button that ended it lifts before LVGL looks again, and that press is nobody's job
    for (int i = 0; i < 100 && (board_button() || board_touch_raw(NULL, NULL, NULL)); i++) vTaskDelay(pdMS_TO_TICKS(20));
    buttons_flush();
    ui_picture_end();
}

static void picture_task(void *arg)
{
    httpd_req_t *req;
    for (;;) {
        if (xQueueReceive(picture_queue, &req, portMAX_DELAY) == pdTRUE) picture_work(req);
    }
}

static esp_err_t on_picture(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!uxQueueSpacesAvailable(picture_queue)) return refuse(req, 409, "a picture is already arriving");
    httpd_req_t *held;
    if (httpd_req_async_handler_begin(req, &held) != ESP_OK) return refuse(req, 500, "could not take the picture");
    xQueueSend(picture_queue, &held, 0);
    return ESP_OK;
}

// --- /screen: a line across the top ----------------------------------------------------------

static esp_err_t on_screen(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *in = read_json(req);
    if (!in) return ESP_OK;
    const cJSON *text = cJSON_GetObjectItem(in, "text");
    const cJSON *secs = cJSON_GetObjectItem(in, "seconds");
    const cJSON *chat = cJSON_GetObjectItem(in, "chat");
    if (cJSON_IsString(chat)) {                                    // {"chat": "kitchen"}: the chat it talks in, on the bar; "" = none
        const cJSON *brain = cJSON_GetObjectItem(in, "brain");     // with it: the model's name and "#rrggbb", the chat's trim
        const cJSON *tint = cJSON_GetObjectItem(in, "trim");
        int rgb = -1;
        if (cJSON_IsString(tint) && tint->valuestring[0] == '#' && strlen(tint->valuestring) == 7) rgb = (int)strtol(tint->valuestring + 1, NULL, 16);
        if (ui_chat(chat->valuestring, cJSON_IsString(brain) ? brain->valuestring : "", rgb) && chat->valuestring[0])
            link_ask_history(-1);                               // a new chat: the page opens on it
        cJSON *out = cJSON_CreateObject();
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "chat", chat->valuestring);
        cJSON_Delete(in);
        return say_json(req, 200, out);
    }
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

static void restart_soon(void *arg) { vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }   // esp_restart runs the shutdown handlers (WiFi off) on this stack

static void sleep_soon(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    board_backlight(0);
    board_led(0, 0, 0);
    esp_sleep_enable_ext0_wakeup(BOARD_BOOT_BUTTON, 0);   // BOOT brings it back
    esp_deep_sleep_start();
}

// PUT /screen/settings {"rotation": 0|90|180|270, "home", "brightness", "invert", "mirror"}: how
// the glass sits, kept. A turn restarts the board to draw that way (a card being written is left to
// finish first); invert and mirror take effect at once, as the console's `screen` words do.
static esp_err_t on_screen_settings(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *in = read_json(req);
    if (!in) return ESP_OK;
    const cJSON *rot = cJSON_GetObjectItem(in, "rotation");
    const cJSON *home = cJSON_GetObjectItem(in, "home");
    const cJSON *bright = cJSON_GetObjectItem(in, "brightness");
    const cJSON *inv = cJSON_GetObjectItem(in, "invert");
    const cJSON *mir = cJSON_GetObjectItem(in, "mirror");
    int want = cJSON_IsNumber(rot) ? (int)rot->valuedouble : -1;
    if (cJSON_IsNumber(rot) && want != 0 && want != 90 && want != 180 && want != 270) { cJSON_Delete(in); return refuse(req, 400, "rotation: 0, 90, 180 or 270"); }
    bool turn = want >= 0 && want != board()->rotation;
    if (cJSON_IsString(home) && !ui_set_home(home->valuestring)) { cJSON_Delete(in); return refuse(req, 400, "home: chat, backups, status, board or actions"); }
    if (cJSON_IsNumber(bright)) {
        int v = (int)bright->valuedouble;
        settings.brightness = v < 10 ? 10 : v > 100 ? 100 : v;
        settings_save();
        board_backlight(settings.brightness);
    }
    if (cJSON_IsBool(inv) && cJSON_IsTrue(inv) != board()->invert) {   // only a change: each set is a flash write
        if (ui_up()) lvgl_port_lock(0);    // the panel takes a command from one task at a time: never under LVGL's flush
        board_set_invert(cJSON_IsTrue(inv));
        if (ui_up()) lvgl_port_unlock();
    }
    if (cJSON_IsBool(mir) && cJSON_IsTrue(mir) != board()->mirror) {
        if (ui_up()) lvgl_port_lock(0);
        board_set_mirror(cJSON_IsTrue(mir));
        if (ui_up()) { lvgl_port_unlock(); ui_repaint(); }   // what is on the glass was drawn the other way
    }
    cJSON_Delete(in);
    if (turn && storage_busy()) return refuse(req, 409, "the card is being written: save again when it is done");
    if (turn) {
        board_set_rotation(want);
        xTaskCreate(restart_soon, "restart", 3072, NULL, 5, NULL);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "rotation", board()->rotation);
    cJSON_AddStringToObject(j, "home", ui_home_name());
    cJSON_AddNumberToObject(j, "brightness", settings.brightness);
    cJSON_AddBoolToObject(j, "invert", board()->invert);
    cJSON_AddBoolToObject(j, "mirror", board()->mirror);
    cJSON_AddBoolToObject(j, "restarting", turn);
    return say_json(req, 200, j);
}

static esp_err_t on_power(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    char action[16];
    if (!query_value(req, "action", action, sizeof action)) return refuse(req, 400, "action=restart or shutdown");
    bool restart = !strcmp(action, "restart");
    if (!restart && strcmp(action, "shutdown")) return refuse(req, 400, "action=restart or shutdown");
    if (storage_busy()) return refuse(req, 409, "a backup is arriving: not now");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddNumberToObject(j, "in_s", 2);
    xTaskCreate(restart ? restart_soon : sleep_soon, restart ? "restart" : "sleep", 3072, NULL, 5, NULL);
    return say_json(req, 200, j);
}

// --- /audio/speak: her sentence, as it arrives, into the DAC (ai-speaker-mic does the same) ----

static void speak_work(httpd_req_t *req)
{
    uint8_t *buf = malloc(SPEAK_CHUNK);
    size_t left = req->content_len, have = 0;
    wav_info_t info = {0};
    bool headed = false, stopped = false;
    int problem = buf ? 0 : 500;
    if (esp_timer_get_time() / 1000 < halted_until_ms) {
        stopped = true;                                  // the rest of a stopped reply: taken, not played
    } else {
        cut = false;
        speaking = true;
        light_cue("speaking");
    }
    while (left > 0 && !problem) {
        int n = httpd_req_recv(req, (char *)buf + have, SPEAK_CHUNK - have);
        if (n <= 0) { problem = 400; break; }
        left -= n;
        have += n;
        if (stopped) { have = 0; continue; }             // drained, so the connection stays clean
        if (cut) { stopped = true; have = 0; continue; }
        if (!headed) {
            if (have < 64 && left > 0) continue;              // a header needs a few bytes
            if (!wav_parse(buf, have, &info) || info.rate != SPEAKER_RATE) { problem = 415; break; }
            headed = true;
            memmove(buf, buf + info.data_at, have - info.data_at);
            have -= info.data_at;
        }
        size_t whole = have & ~(size_t)(2 * info.channels - 1);     // whole frames only
        speaker_play((int16_t *)buf, whole / 2, info.channels);
        memmove(buf, buf + whole, have - whole);
        have -= whole;
    }
    if (speaking) speaker_play_end();
    speaking = false;
    free(buf);
    light_cue("idle");
    if (problem == 415) refuse(req, 415, "not a 22050 Hz 16 bit PCM WAV");
    else if (problem == 500) refuse(req, 500, "no memory for the sound");
    else if (problem) refuse(req, 400, "the sound did not arrive whole");
    else {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        if (stopped) cJSON_AddBoolToObject(j, "stopped", true);
        say_json(req, 200, j);
    }
    httpd_req_async_handler_complete(req);
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
        return refuse(req, 415, "this board plays audio/wav, 22050 Hz, one channel, 16 bit");
    if (!speaker_ok()) return refuse(req, 500, "the speaker is not running on this board");
    if (!uxQueueSpacesAvailable(speak_queue)) return refuse(req, 409, "already playing something, with one more waiting");
    httpd_req_t *held;
    if (httpd_req_async_handler_begin(req, &held) != ESP_OK) return refuse(req, 500, "could not take the sound");
    xQueueSend(speak_queue, &held, 0);
    return ESP_OK;
}

static esp_err_t on_stop(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    bool was = speaking;
    cut = true;
    halted_until_ms = esp_timer_get_time() / 1000 + HALT_MS;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "stopped", was);
    return say_json(req, 200, j);
}

static esp_err_t on_volume(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (req->method == HTTP_POST) {
        char text[8];
        if (!query_value(req, "level", text, sizeof text)) return refuse(req, 400, "level=0..100");
        speaker_set_volume(atoi(text));
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "volume", speaker_volume());
    return say_json(req, 200, j);
}

static esp_err_t on_sounds(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddArrayToObject(j, "sounds");             // none stored on this board yet
    return say_json(req, 200, j);
}

// --- /storage: the card as a place she keeps backups (same doors as the audio board) ----

static const char *file_name(httpd_req_t *req) { return req->uri + strlen("/storage/"); }

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
            if (!storage_path(e->d_name, path, sizeof path) || stat(path, &st) || !S_ISREG(st.st_mode)) continue;
            cJSON *f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "name", e->d_name);
            cJSON_AddNumberToObject(f, "size", (double)st.st_size);
            cJSON_AddNumberToObject(f, "mtime", (double)st.st_mtime);
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
        bool cut = storage_cancelled();
        storage_write_drop(w);
        if (cut) refuse(req, 409, "cancelled on the board");
        else if (problem == 422) refuse(req, 400, "not a sealed backup: plaintext never lands on this card");
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
    if (!uxQueueSpacesAvailable(store_queue)) return refuse_unread(req, 409, "the card is busy");
    store_job_t job = {.work = work};
    if (httpd_req_async_handler_begin(req, &job.req) != ESP_OK) return refuse_unread(req, 500, "could not take it");
    xQueueSend(store_queue, &job, 0);
    return ESP_OK;
}

// PUT /firmware: her program over the air, written by ota.c on the store
// task (one body at a time there: a backup and a program never race).
static void firmware_put(httpd_req_t *req) { ota_put(req, STORE_PACE_MS, NULL); }

static esp_err_t on_firmware(httpd_req_t *req)
{
    if (!let_in(req)) return req->content_len > UNREAD_BIG ? ESP_FAIL : ESP_OK;
    return hand_to_store(req, firmware_put);
}

static esp_err_t on_storage_file(httpd_req_t *req)
{
    if (!let_in(req)) return req->content_len > UNREAD_BIG ? ESP_FAIL : ESP_OK;
    if (!storage_alive()) return refuse_unread(req, 503, "no card");
    const char *name = file_name(req);
    if (!storage_name_ok(name)) return refuse_unread(req, 400, "bad name");
    if (req->method == HTTP_DELETE) {
        if (!storage_remove(name)) return refuse(req, 404, "no such file, or the card is busy");   // and the count on the card is told again
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "ok", true);
        return say_json(req, 200, j);
    }
    if (req->method == HTTP_GET) return hand_to_store(req, store_get);
    if (!req->content_len) return refuse(req, 411, "Content-Length required");
    size_t n = strlen(name);
    bool sealed_name = n > 12 && !strcmp(name + n - 12, ".sapphirebak");
    if (!sealed_name && !storage_name_plain_ok(name)) return refuse_unread(req, 400, "only sealed backups (.sapphirebak) land here");
    uint64_t free_b = 0;
    if (storage_room(&free_b, NULL) && (uint64_t)req->content_len + (16u << 20) > free_b)
        return refuse_unread(req, 507, "not enough room on the card");
    if (storage_busy()) return refuse_unread(req, 409, "another backup is arriving");
    return hand_to_store(req, store_put);
}

// POST /storage/cancel: the backup arriving now is dropped (the Backups page has the same button)
static esp_err_t on_storage_cancel(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!storage_cancel()) return refuse(req, 409, "nothing is arriving");
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    return say_json(req, 200, j);
}

// POST /screen/calibrate: three crosses on the glass, 60 s (a resistive glass; the Devices page's Recalibrate)
static esp_err_t on_screen_calibrate(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!board_touch_needs_cal()) return refuse(req, 400, "a capacitive glass needs no calibration");
    ui_calibrate();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddNumberToObject(j, "seconds", 60);
    return say_json(req, 200, j);
}

static esp_err_t on_storage_format(httpd_req_t *req)
{
    if (!let_in(req)) return ESP_OK;
    if (!storage_alive()) return refuse(req, 503, "no card");
    if (storage_busy()) return refuse(req, 409, "a backup is arriving");
    return hand_to_store(req, store_format);
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
    picture_queue = xQueueCreate(1, sizeof(httpd_req_t *));
    xTaskCreatePinnedToCore(picture_task, "picture", 4096, NULL, 5, NULL, 1);
    speak_queue = xQueueCreate(2, sizeof(httpd_req_t *));
    xTaskCreatePinnedToCore(speak_task, "speak", 4096, NULL, 6, NULL, 1);
    store_queue = xQueueCreate(1, sizeof(store_job_t));
    xTaskCreatePinnedToCore(store_task, "store", 4096, NULL, 3, NULL, 1);
    const httpd_uri_t routes[] = {              // exact routes first; the /storage/* wildcard last
        {"/health", HTTP_GET, on_health, NULL},
        {"/led", HTTP_POST, on_led, NULL},
        {"/led/spec", HTTP_GET, on_led_spec, NULL},
        {"/led/baseline", HTTP_GET, on_led_baseline, NULL},
        {"/led/baseline", HTTP_PUT, on_led_baseline, NULL},
        {"/led/looks", HTTP_PUT, on_led_looks, NULL},
        {"/screen", HTTP_POST, on_screen, NULL},
        {"/screen/settings", HTTP_PUT, on_screen_settings, NULL},
        {"/screen/picture", HTTP_POST, on_picture, NULL},
        {"/power", HTTP_POST, on_power, NULL},
        {"/audio/speak", HTTP_POST, on_speak, NULL},
        {"/audio/stop", HTTP_POST, on_stop, NULL},
        {"/volume", HTTP_GET, on_volume, NULL},
        {"/volume", HTTP_POST, on_volume, NULL},
        {"/sounds", HTTP_GET, on_sounds, NULL},
        {"/sensors", HTTP_GET, on_sensors, NULL},
        {"/storage", HTTP_GET, on_storage, NULL},
        {"/firmware", HTTP_PUT, on_firmware, NULL},
        {"/storage/format", HTTP_POST, on_storage_format, NULL},
        {"/storage/cancel", HTTP_POST, on_storage_cancel, NULL},
        {"/screen/calibrate", HTTP_POST, on_screen_calibrate, NULL},
        {"/storage/*", HTTP_PUT, on_storage_file, NULL},
        {"/storage/*", HTTP_GET, on_storage_file, NULL},
        {"/storage/*", HTTP_DELETE, on_storage_file, NULL},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) httpd_register_uri_handler(server, &routes[i]);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, on_missing);
    ESP_LOGI(TAG, "listening on port 80");
}
