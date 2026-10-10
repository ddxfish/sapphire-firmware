// net.c - joins the WiFi in the settings and keeps joining it. Prints
// "POCKET IP x.x.x.x" on the serial line when it has an address: the setup
// script waits for that line.
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"

#include "light.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "net";
static char ip_text[20];
static bool joined;
static net_up_fn up_fn;
static bool inited;              // esp_netif and esp_wifi are up: net_start, or a scan on a board not yet set up

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        joined = false;
        ip_text[0] = 0;
        ESP_LOGW(TAG, "WiFi lost, joining again in 3 s");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = data;
    snprintf(ip_text, sizeof ip_text, IPSTR, IP2STR(&ev->ip_info.ip));
    joined = true;
    ESP_LOGI(TAG, "on WiFi at %s", ip_text);
    printf("POCKET IP %s\n", ip_text);
    fflush(stdout);
    if (up_fn) up_fn();
}

static void wifi_init(void)
{
    if (inited) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, settings.name);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    inited = true;
}

// The networks around, strongest first, into a JSON array of {ssid, rssi}:
// the flasher offers them as a list. On a board not yet set up the radio is
// brought up for this alone, joining nothing; the restart after `setup`
// starts it properly. Returns how many, or -1 when it cannot scan now.
int net_scan(cJSON *list)
{
    if (!inited) {
        if (settings_ready()) return -1;        // net_start is about to run: ask again in a moment
        wifi_init();
        if (esp_wifi_start() != ESP_OK) return -1;
    }
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;    // blocks about 2 s
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 24) n = 24;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof *recs);
    if (!recs) { esp_wifi_clear_ap_list(); return -1; }
    esp_wifi_scan_get_ap_records(&n, recs);
    int kept = 0;
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        bool dup = !ssid[0];
        for (int k = 0; k < i && !dup; k++) dup = !strcmp(ssid, (const char *)recs[k].ssid);
        if (dup) continue;                      // one line per network, its strongest
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddItemToArray(list, o);
        kept++;
    }
    free(recs);
    return kept;
}

void net_start(net_up_fn on_up)
{
    up_fn = on_up;
    wifi_init();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, settings.wifi_ssid, sizeof cfg.sta.ssid);
    strlcpy((char *)cfg.sta.password, settings.wifi_password, sizeof cfg.sta.password);
    cfg.sta.threshold.authmode = settings.wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));     // a pocket box may run on a battery one day
    ESP_ERROR_CHECK(esp_wifi_start());

    if (mdns_init() == ESP_OK) {                              // <name>.local
        mdns_hostname_set(settings.name);
        mdns_instance_name_set("Sapphire pocket");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
}

const char *net_ip(void) { return ip_text; }
bool net_up(void) { return joined; }

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return joined && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
