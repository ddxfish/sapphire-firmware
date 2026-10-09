// net.h - WiFi. Joins what the settings say and keeps joining it.
#pragma once
#include <stdbool.h>

#include "cJSON.h"

typedef void (*net_up_fn)(void);

void net_start(net_up_fn on_up);       // on_up runs each time an address arrives
const char *net_ip(void);              // "" until joined
bool net_up(void);
int net_rssi(void);                    // dBm, 0 when not joined
int net_scan(cJSON *list);             // the networks around, {ssid, rssi} each, strongest first; -1 = not now
