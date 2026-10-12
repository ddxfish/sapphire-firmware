// ota.c - her program, over the air. Shared by every board (copied, like
// storage.c). PUT /firmware takes a program as the body: it is written into
// the slot not running, checked (image, then sha256 against X-Sha256), marked
// to boot, and the board restarts onto it. The bootloader's rollback keeps
// the old program if the new one never says it is fine: ota_fine() when WiFi
// joins, or FINE_AFTER_S seconds up without a crash (a board set up away
// from its WiFi still keeps a good program).
#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#define OTA_CHUNK 4096
#define FINE_AFTER_S 90

static const char *TAG = "ota";
static bool said_fine;

bool ota_can(void) { return esp_ota_get_next_update_partition(NULL) != NULL; }

const char *ota_slot(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

void ota_fine(void)
{
    if (said_fine) return;
    said_fine = true;
    esp_ota_mark_app_valid_cancel_rollback();      // nothing to do unless this program is on trial
}

static void fine_later(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(FINE_AFTER_S * 1000));
    ota_fine();
    ESP_LOGI(TAG, "this program ran %d s: kept", FINE_AFTER_S);
    vTaskDelete(NULL);
}

void ota_start(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "a new program on trial in slot %s: kept once WiFi joins or %d s pass", run->label, FINE_AFTER_S);
        xTaskCreate(fine_later, "ota-fine", 4096, NULL, 2, NULL);   // it writes flash and logs: on 2 KB a fall here would roll a good program back
    } else {
        said_fine = true;
    }
}

static void answer(httpd_req_t *req, int code, cJSON *j)
{
    char status[40];
    snprintf(status, sizeof status, "%d %s", code, code == 200 ? "OK" : "Refused");
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char *text = cJSON_PrintUnformatted(j);
    httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(j);
}

static void refuse(httpd_req_t *req, int code, const char *why)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "detail", why);
    answer(req, code, j);
    ESP_LOGW(TAG, "refused: %s", why);
}

static void restart_soon(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void ota_put(httpd_req_t *req, int pace_ms, void (*yield)(void))
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t ota = 0;
    uint8_t *buf = NULL;
    if (!next) { refuse(req, 501, "this board has one program slot: update it over USB"); goto done; }
    if (req->content_len < 4096 || req->content_len > next->size) { refuse(req, 413, "that program does not fit the slot"); goto done; }
    if (esp_ota_begin(next, req->content_len, &ota) != ESP_OK) { refuse(req, 507, "could not open the program slot"); goto done; }
    buf = malloc(OTA_CHUNK);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    size_t left = req->content_len;
    int problem = buf ? 0 : 507;
    bool first = true;
    while (left > 0 && !problem) {
        if (yield) yield();
        int n = httpd_req_recv(req, (char *)buf, left < OTA_CHUNK ? left : OTA_CHUNK);
        if (n <= 0) { problem = 400; break; }
        if (first) {
            first = false;
            if (buf[0] != 0xE9) { problem = 422; break; }       // an ESP image starts so
            // ... and says whose program it is, 80 bytes in (esp_app_desc_t.project_name). Another
            // board's program for the same chip passed every check, ran, and was kept.
            if (n >= 112 && strncmp((char *)buf + 80, esp_app_get_description()->project_name, 32)) { problem = 409; break; }
        }
        if (esp_ota_write(ota, buf, n) != ESP_OK) { problem = 507; break; }
        mbedtls_sha256_update(&sha, buf, n);
        left -= n;
        if (pace_ms) vTaskDelay(pdMS_TO_TICKS(pace_ms));
    }
    free(buf);
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (problem) {
        esp_ota_abort(ota);
        refuse(req, problem == 409 ? 422 : problem, problem == 422 ? "not an ESP32 program" : problem == 409 ? "a program for another kind of board"
               : problem == 507 ? "the slot could not be written"
               : "the program did not arrive whole");
        goto done;
    }
    if (esp_ota_end(ota) != ESP_OK) { refuse(req, 422, "the program did not check out: a bad or cut image"); goto done; }
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    char want[72] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Sha256", want, sizeof want) == ESP_OK && want[0] && strcasecmp(want, hex)) {
        refuse(req, 422, "sha256 mismatch: not installed");
        goto done;
    }
    if (esp_ota_set_boot_partition(next) != ESP_OK) { refuse(req, 507, "could not mark the new program to boot"); goto done; }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    cJSON_AddBoolToObject(j, "restarting", true);
    cJSON_AddStringToObject(j, "slot", next->label);
    cJSON_AddStringToObject(j, "sha256", hex);
    answer(req, 200, j);
    ESP_LOGI(TAG, "new program in slot %s (%u bytes): restarting onto it", next->label, (unsigned)req->content_len);
    xTaskCreate(restart_soon, "ota-restart", 3072, NULL, 5, NULL);
done:
    httpd_req_async_handler_complete(req);
}
