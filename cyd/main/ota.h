// ota.h - her program, over the air (see ota.c). The protocol's `firmware`
// capability: /health says `has: firmware` when ota_can(), plus `slot`.
#pragma once
#include <stdbool.h>

#include "esp_http_server.h"

void ota_start(void);                  // at boot: a program on trial is kept after FINE_AFTER_S
void ota_fine(void);                   // this program works (WiFi joined): keep it, no rollback
bool ota_can(void);                    // two program slots on this board
const char *ota_slot(void);            // the slot running now: "ota_0"
void ota_put(httpd_req_t *req, int pace_ms, void (*yield)(void));   // PUT /firmware, on an async-handed request
