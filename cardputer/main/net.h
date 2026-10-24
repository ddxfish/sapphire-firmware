// net.h - WiFi as a station, the board's own name on the network (mDNS).
#pragma once
#include <stdbool.h>

#include "cJSON.h"

typedef void (*net_up_fn)(void);

void net_start(net_up_fn on_up);       // on_up is called once each time an address arrives
const char *net_ip(void);              // "192.168.0.50", or "" while not joined
int net_rssi(void);                    // dBm, 0 while not joined
bool net_up(void);
int net_scan(cJSON *list);             // the networks around, {ssid, rssi} each, strongest first; -1 = not now
