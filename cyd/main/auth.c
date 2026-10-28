// auth.c - see auth.h. Runs on the web server's one task only.
#include "auth.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "mbedtls/md.h"

#define SEEN 96                        // signatures kept to refuse a second use; one pushed out raises the floor

typedef struct { int64_t ts; uint64_t n; } seen_t;
static seen_t *seen;
static int seen_at;
static int64_t floor_ts;               // a signature at or under this may have been pushed out: refused

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void auth_board_time(httpd_req_t *req)
{
    static char text[24];              // httpd keeps the pointer until the answer is sent
    snprintf(text, sizeof text, "%" PRId64, now_ms());
    httpd_resp_set_hdr(req, "X-Board-Time", text);
}

static bool hex_same(const char *a, const char *b, size_t n)
{
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned)(a[i] | 0x20) ^ (unsigned)(b[i] | 0x20);
    return diff == 0;
}

static bool field(const char *header, const char *name, char *out, size_t size)
{
    const char *at = strstr(header, name);
    if (!at) return false;
    at += strlen(name);
    size_t n = strcspn(at, ", ");
    if (!n || n >= size) return false;
    memcpy(out, at, n);
    out[n] = 0;
    return true;
}

bool auth_signed(httpd_req_t *req, const char *header, const char *key)
{
    char ts_text[24], nonce[20], sig[72], sha[72] = {0};
    if (strncmp(header, "Sapphire ", 9) || !key[0]
        || !field(header, " ts=", ts_text, sizeof ts_text) || !field(header, ",n=", nonce, sizeof nonce)
        || !field(header, ",sig=", sig, sizeof sig) || strlen(sig) != 64 || strlen(nonce) != 16) return false;
    char *end;
    int64_t ts = strtoll(ts_text, &end, 10);
    uint64_t n = strtoull(nonce, NULL, 16);
    int64_t now = now_ms();
    if (*end || ts <= floor_ts || ts < now - AUTH_WINDOW_MS || ts > now + AUTH_WINDOW_MS) return false;
    if (!seen && !(seen = heap_caps_calloc(SEEN, sizeof *seen, MALLOC_CAP_SPIRAM)) && !(seen = calloc(SEEN, sizeof *seen))) return false;
    for (int i = 0; i < SEEN; i++) if (seen[i].n == n && seen[i].ts == ts) return false;
    httpd_req_get_hdr_value_str(req, "X-Sha256", sha, sizeof sha);
    char text[700];
    int len = snprintf(text, sizeof text, "%s\n%s\n%s\n%s\n%s", http_method_str(req->method), req->uri, ts_text, nonce, sha);
    if (len < 0 || len >= (int)sizeof text) return false;
    unsigned char mac[32];
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char *)key, strlen(key),
                        (const unsigned char *)text, len, mac)) return false;
    char want[65];
    for (int i = 0; i < 32; i++) sprintf(want + 2 * i, "%02x", mac[i]);
    if (!hex_same(want, sig, 64)) return false;
    if (seen[seen_at].ts > floor_ts) floor_ts = seen[seen_at].ts;
    seen[seen_at] = (seen_t){ts, n};
    seen_at = (seen_at + 1) % SEEN;
    return true;
}
