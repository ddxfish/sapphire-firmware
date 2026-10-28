// auth.h - a request signed with the board's key, so the key itself never
// travels (0.9.4). Sapphire's calls to the board are plain HTTP on the LAN: a
// Bearer key there was readable by anyone on the WiFi, and with it came every
// door, a new program among them (scouts, 2026-10-28).
//
//   Authorization: Sapphire ts=<unix ms>,n=<16 hex>,sig=<64 hex>
//   sig = hex(HMAC-SHA256(key, METHOD "\n" URI "\n" ts "\n" n "\n" X-Sha256))
//
// The URI is the path with its query, as sent; X-Sha256 is the header's value
// or empty, so a program or a backup is signed by its hash. ts is the sender's
// clock fitted to this board's: a refusal carries X-Board-Time (this board's
// unix ms) and the sender retries once with the difference, so the board needs
// no clock of its own. A signature is good once, within AUTH_WINDOW_MS.
// Only the key is protected: what is sent and answered is still readable.
#pragma once
#include <stdbool.h>

#include "esp_http_server.h"

#define AUTH_WINDOW_MS 30000

bool auth_signed(httpd_req_t *req, const char *header, const char *key);
void auth_board_time(httpd_req_t *req);       // X-Board-Time on a 401
