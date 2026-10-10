// storage.c - see storage.h. The card over SDMMC (1-bit, GPIO matrix) or,
// when board.h says BOARD_SD_SPI, over a plain SPI bus; FATFS either way.
// The same file serves every board: only the mount differs (2026-10-06).
#include "storage.h"
#include "board.h"                     // first: it says how the card is wired

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef BOARD_SD_SPI
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#else
#include "driver/sdmmc_host.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

#define MOUNT "/sdcard"
#define FOLDER MOUNT "/sapphire-backups"
#ifndef BOARD_STORAGE_CAPS             // where the write buffer lives: PSRAM on a board that has it, so
#define BOARD_STORAGE_CAPS MALLOC_CAP_SPIRAM   // internal RAM stays free for TLS (2026-10-06)
#endif
#ifndef BOARD_STORAGE_CHUNK
#define BOARD_STORAGE_CHUNK (16 * 1024)        // one card write; the driver bounces it through its own DMA buffer
#endif
#define WRITE_CHUNK BOARD_STORAGE_CHUNK

#define HASH_END ".sha256"             // beside each backup, when the board keeps hashes: 64 hex characters
#define DAMAGED_MAX 8
#define BUSY_WAIT_MS 10000             // a write waits this long for the check to step aside

static sdmmc_card_t *card;
static bool mounted;
static SemaphoreHandle_t busy;         // one write, one format, one removal or one file being checked at a time
static volatile bool check_holds;      // ... and it is the check that holds it: not "busy" to a caller
static volatile bool step_aside;       // a write or a removal is waiting: the check lets go of its file
static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
static storage_state_t state;
static char damaged[DAMAGED_MAX][STORAGE_NAME_MAX + 1];

static const char *PLAIN_OK[] = {"README.txt", "open-backup.sh", "open-backup.bat", "decrypt_backup.py", NULL};

struct storage_write {
    FILE *f;
    char name[STORAGE_NAME_MAX + 1];
    char part[sizeof FOLDER + STORAGE_NAME_MAX + 16];
    uint8_t *buf;
    size_t have;
    uint64_t expect, got;
    mbedtls_sha256_context sha;
};

// --- what is on the card -------------------------------------------------------------

static bool ends(const char *name, const char *end)
{
    size_t n = strlen(name), m = strlen(end);
    return n > m && !strcmp(name + n - m, end);
}

static bool sealed(const char *name) { return ends(name, ".sapphirebak"); }

static int damaged_at(const char *name)
{
    for (int i = 0; i < DAMAGED_MAX; i++) if (damaged[i][0] && !strcmp(damaged[i], name)) return i;
    return -1;
}

static void mark_damaged(const char *name)
{
    if (damaged_at(name) >= 0) return;
    for (int i = 0; i < DAMAGED_MAX; i++) if (!damaged[i][0]) { strlcpy(damaged[i], name, sizeof damaged[i]); return; }
}

// Counts the backups again: after the mount, a write, a removal, a format, the check.
static void rescan(void)
{
    int count = 0, unchecked = 0, bad = 0;
    char newest[STORAGE_NAME_MAX + 1] = "";
    DIR *d = mounted ? opendir(FOLDER) : NULL;
    if (d) {
        struct dirent *e;
        char path[sizeof FOLDER + 300];
        struct stat st;
        while ((e = readdir(d))) {
            if (!sealed(e->d_name) || strlen(e->d_name) > STORAGE_NAME_MAX) continue;
            count++;
            if (strcmp(e->d_name, newest) > 0) strlcpy(newest, e->d_name, sizeof newest);   // the name starts with its date
            if (damaged_at(e->d_name) >= 0) { bad++; continue; }
            snprintf(path, sizeof path, FOLDER "/%s" HASH_END, e->d_name);
            if (stat(path, &st)) unchecked++;
        }
        closedir(d);
    }
    taskENTER_CRITICAL(&state_mux);
    state.count = count;
    state.unchecked = unchecked;
    state.damaged = bad;
    strlcpy(state.newest, newest, sizeof state.newest);
    taskEXIT_CRITICAL(&state_mux);
}

void storage_state(storage_state_t *out)
{
    taskENTER_CRITICAL(&state_mux);
    *out = state;
    taskEXIT_CRITICAL(&state_mux);
    out->mounted = mounted;
}

int storage_checked(const char *name)
{
    if (damaged_at(name) >= 0) return 0;
    char path[sizeof FOLDER + 300];
    struct stat st;
    snprintf(path, sizeof path, FOLDER "/%s" HASH_END, name);
    return stat(path, &st) ? -1 : 1;
}

// The mutex for a writer: the check is asked to step aside first.
static bool take_busy(void)
{
    step_aside = true;
    bool got = xSemaphoreTake(busy, check_holds ? pdMS_TO_TICKS(BUSY_WAIT_MS) : 0) == pdTRUE;
    step_aside = false;
    return got;
}

// sha256 of a file as the card gives it back. `yielding`: give up when a writer is waiting.
static bool hash_file(const char *path, char hex[65], bool yielding)
{
    int fd = open(path, O_RDONLY);                         // read() straight from the card: stdio halves the speed
    uint8_t *buf = fd >= 0 ? heap_caps_malloc(WRITE_CHUNK, BOARD_STORAGE_CAPS) : NULL;
    if (!buf) { if (fd >= 0) close(fd); return false; }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    ssize_t n;
    bool ok = true;
    while ((n = read(fd, buf, WRITE_CHUNK)) > 0) {
        mbedtls_sha256_update(&sha, buf, n);
        if (yielding && step_aside) { ok = false; break; }
        if (yielding) vTaskDelay(1);                       // the check is nobody's hurry
    }
    ok = ok && n == 0;
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    free(buf);
    close(fd);
    return ok;
}

#ifdef BOARD_STORAGE_CHECKS
static bool keep_hash(const char *name, const char *hex)
{
    char path[sizeof FOLDER + 300];
    snprintf(path, sizeof path, FOLDER "/%s" HASH_END, name);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(hex, 1, 64, f) == 64 && !fflush(f) && !fsync(fileno(f));
    return !fclose(f) && ok;
}

// Every backup against the hash kept beside it, once after the card is
// mounted: what a pulled plug or a tired card did to them is found here, not
// on the day one is needed. One file at a time, and a write goes first.
static volatile bool check_on;         // one check at a time: at the start, or asked for (storage_check_now)

static void check_task(void *arg)
{
    char (*names)[STORAGE_NAME_MAX + 1] = NULL;
    int n = 0, room = 0;
    DIR *d = opendir(FOLDER);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!sealed(e->d_name) || strlen(e->d_name) > STORAGE_NAME_MAX) continue;
        if (n == room) {
            void *more = realloc(names, (room + 16) * sizeof *names);
            if (!more) break;
            names = more;
            room += 16;
        }
        strlcpy(names[n++], e->d_name, sizeof *names);
    }
    if (d) closedir(d);
    taskENTER_CRITICAL(&state_mux);
    state.checking = n > 0;
    state.checked = 0;
    state.to_check = n;
    taskEXIT_CRITICAL(&state_mux);
    int bad = 0;
    for (int i = 0; i < n; ) {
        char path[sizeof FOLDER + 300], want[65] = "", got[65];
        while (step_aside || xSemaphoreTake(busy, pdMS_TO_TICKS(200)) != pdTRUE) vTaskDelay(pdMS_TO_TICKS(500));
        check_holds = true;
        snprintf(path, sizeof path, FOLDER "/%s" HASH_END, names[i]);
        FILE *f = fopen(path, "rb");
        size_t have = f ? fread(want, 1, 64, f) : 0;
        if (f) fclose(f);
        snprintf(path, sizeof path, FOLDER "/%s", names[i]);
        struct stat st;
        bool there = !stat(path, &st), again = false;
        if (there && have == 64) {
            if (hash_file(path, got, true)) {
                if (strcmp(got, want)) { mark_damaged(names[i]); bad++; ESP_LOGE(TAG, "%s is DAMAGED: it no longer matches its hash", names[i]); }
            } else again = step_aside;                       // a writer came: this one once more, after it
        }
        check_holds = false;
        xSemaphoreGive(busy);
        if (again) continue;
        i++;
        taskENTER_CRITICAL(&state_mux);
        state.checked = i;
        taskEXIT_CRITICAL(&state_mux);
    }
    free(names);
    rescan();
    taskENTER_CRITICAL(&state_mux);
    state.checking = false;
    taskEXIT_CRITICAL(&state_mux);
    ESP_LOGI(TAG, "checked %d backup(s): %d damaged, %d without a hash", n, bad, state.unchecked);
    check_on = false;
    vTaskDelete(NULL);
}

bool storage_check_now(void)
{
    if (!mounted || check_on) return false;
    check_on = true;
    if (xTaskCreate(check_task, "card-check", 4096, NULL, 2, NULL) == pdPASS) return true;
    check_on = false;
    return false;
}
#endif

// --- mount -------------------------------------------------------------------------

static void sweep_partials(void)
{
    DIR *d = opendir(FOLDER);
    if (!d) return;
    struct dirent *e;
    char path[sizeof FOLDER + 300];
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 8 && !strcmp(e->d_name + n - 8, ".partial")) {
            snprintf(path, sizeof path, FOLDER "/%s", e->d_name);
            if (!unlink(path)) ESP_LOGW(TAG, "dropped a half-written %s", e->d_name);
        }
        if (ends(e->d_name, HASH_END)) {                     // a hash whose backup is gone
            struct stat st;
            snprintf(path, sizeof path, FOLDER "/%.*s", (int)(n - strlen(HASH_END)), e->d_name);
            if (stat(path, &st)) { snprintf(path, sizeof path, FOLDER "/%s", e->d_name); unlink(path); }
        }
    }
    closedir(d);
}

bool storage_start(void)
{
    if (mounted) return true;
    if (!busy) busy = xSemaphoreCreateMutex();
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,                   // never wipe a person's card on our own
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
#ifdef BOARD_SD_SPI
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_SD_MOSI, .miso_io_num = BOARD_SD_MISO, .sclk_io_num = BOARD_SD_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(BOARD_SD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { ESP_LOGW(TAG, "the card's SPI bus did not start"); return false; }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SD_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = BOARD_SD_CS;
    slot.host_id = BOARD_SD_HOST;
    err = esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot, &cfg, &card);
#else
    if (!board_sd_select(true)) { ESP_LOGW(TAG, "no expander, so no card"); return false; }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;              // 20 MHz; one line is still faster than WiFi
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = BOARD_SD_CLK;
    slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0;
    slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;             // D3/CS is on the expander, held high
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &cfg, &card);
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no card mounted (%s): backups off until one is in and FAT32", esp_err_to_name(err));
        return false;
    }
    mounted = true;
    mkdir(FOLDER, 0777);
    sweep_partials();
    rescan();
#ifdef BOARD_STORAGE_CHECKS
    storage_check_now();
#endif
    uint64_t total = 0, free_b = 0;
    storage_room(&free_b, &total);
    ESP_LOGI(TAG, "card %s, %llu MB, %llu MB free, backups in %s",
             card->cid.name, (unsigned long long)(total >> 20), (unsigned long long)(free_b >> 20), FOLDER);
    return true;
}

bool storage_ok(void) { return mounted; }

const char *storage_dir(void) { return FOLDER; }

bool storage_room(uint64_t *free_bytes, uint64_t *total_bytes)
{
    if (!mounted) return false;
    uint64_t t = 0, f = 0;
    if (esp_vfs_fat_info(MOUNT, &t, &f) != ESP_OK) return false;
    if (free_bytes) *free_bytes = f;
    if (total_bytes) *total_bytes = t;
    return true;
}

// --- names -------------------------------------------------------------------------

bool storage_name_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (!n || n > STORAGE_NAME_MAX || name[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                  || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool storage_name_plain_ok(const char *name)
{
    for (int i = 0; PLAIN_OK[i]; i++) if (!strcmp(name, PLAIN_OK[i])) return true;
    return false;
}

bool storage_path(const char *name, char *out, size_t size)
{
    if (!storage_name_ok(name)) return false;
    return snprintf(out, size, FOLDER "/%s", name) < (int)size;
}

// --- writing -----------------------------------------------------------------------

bool storage_busy(void)
{
    if (!busy) return false;
    if (xSemaphoreTake(busy, 0) != pdTRUE) return !check_holds;   // the check steps aside for a writer
    xSemaphoreGive(busy);
    return false;
}

storage_write_t *storage_write_begin(const char *name, uint64_t expect)
{
    if (!mounted || !storage_name_ok(name)) return NULL;
    if (!take_busy()) return NULL;
    storage_write_t *w = heap_caps_calloc(1, sizeof *w, BOARD_STORAGE_CAPS);
    if (!w) { xSemaphoreGive(busy); return NULL; }
    w->buf = heap_caps_malloc(WRITE_CHUNK, BOARD_STORAGE_CAPS);
    strlcpy(w->name, name, sizeof w->name);
    snprintf(w->part, sizeof w->part, FOLDER "/%s.partial", name);
    w->f = w->buf ? fopen(w->part, "wb") : NULL;
    if (!w->f) {
        ESP_LOGE(TAG, "could not open %s: %s", w->part, strerror(errno));
        free(w->buf);
        free(w);
        xSemaphoreGive(busy);
        return NULL;
    }
    w->expect = expect;
#ifdef BOARD_STORAGE_CHECKS
    // The file's whole room on the card, taken now: the card's table is
    // written here and at the end, not all through the minutes of the write,
    // so a pulled plug has two short moments to hit it instead of all of them.
    if (expect > 1 && (fseek(w->f, (long)(expect - 1), SEEK_SET) || fputc(0, w->f) == EOF || fflush(w->f) || fsync(fileno(w->f))))
        ESP_LOGW(TAG, "could not take the room first (%s): written the plain way", strerror(errno));
    rewind(w->f);
#endif
    taskENTER_CRITICAL(&state_mux);
    state.writing = true;
    state.got = 0;
    state.expect = expect;
    taskEXIT_CRITICAL(&state_mux);
    mbedtls_sha256_init(&w->sha);
    mbedtls_sha256_starts(&w->sha, 0);
    return w;
}

static bool flush(storage_write_t *w)
{
    if (!w->have) return true;
    size_t n = fwrite(w->buf, 1, w->have, w->f);
    bool ok = n == w->have;
    w->have = 0;
    return ok;
}

bool storage_write_feed(storage_write_t *w, const uint8_t *data, size_t n)
{
    mbedtls_sha256_update(&w->sha, data, n);
    w->got += n;
    taskENTER_CRITICAL(&state_mux);
    state.got = w->got;
    taskEXIT_CRITICAL(&state_mux);
    while (n) {
        size_t take = WRITE_CHUNK - w->have;
        if (take > n) take = n;
        memcpy(w->buf + w->have, data, take);
        w->have += take;
        data += take;
        n -= take;
        if (w->have == WRITE_CHUNK && !flush(w)) return false;
    }
    return true;
}

static void close_free(storage_write_t *w, bool ok)
{
    if (w->f) fclose(w->f);
    bool backup = sealed(w->name);
    rescan();
    taskENTER_CRITICAL(&state_mux);
    state.writing = false;
    if (backup) state.failed = !ok;                        // the last backup sent here did not land
    taskEXIT_CRITICAL(&state_mux);
    mbedtls_sha256_free(&w->sha);
    free(w->buf);
    free(w);
    xSemaphoreGive(busy);
}

bool storage_write_finish(storage_write_t *w, char sha_hex[65], uint64_t *size)
{
    bool ok = flush(w) && w->got == w->expect && !fflush(w->f) && !fsync(fileno(w->f));
    unsigned char digest[32];
    mbedtls_sha256_finish(&w->sha, digest);
    for (int i = 0; i < 32; i++) sprintf(sha_hex + 2 * i, "%02x", digest[i]);
    fclose(w->f);
    w->f = NULL;
    char final[sizeof w->part];
    snprintf(final, sizeof final, FOLDER "/%s", w->name);
#ifdef BOARD_STORAGE_CHECKS
    if (ok && sealed(w->name)) {
        // what the card holds, read back: the hash above is only of what arrived
        char back[65];
        ok = hash_file(w->part, back, false) && !strcmp(back, sha_hex);
        if (!ok) ESP_LOGE(TAG, "%s did not read back as it was written: dropped", w->name);
        ok = ok && keep_hash(w->name, sha_hex);
    }
#endif
    if (ok) {
        unlink(final);                                     // FAT rename does not replace
        ok = rename(w->part, final) == 0;
    }
    if (!ok) unlink(w->part);
    if (ok) { int at = damaged_at(w->name); if (at >= 0) damaged[at][0] = 0; }
    if (size) *size = w->got;
    close_free(w, ok);
    return ok;
}

void storage_write_drop(storage_write_t *w)
{
    if (w->f) { fclose(w->f); w->f = NULL; }
    unlink(w->part);
    close_free(w, false);
}

bool storage_remove(const char *name)
{
    char path[sizeof FOLDER + 300];
    if (!mounted || !storage_path(name, path, sizeof path) || !take_busy()) return false;
    bool ok = unlink(path) == 0;
    snprintf(path, sizeof path, FOLDER "/%s" HASH_END, name);
    unlink(path);
    int at = damaged_at(name);
    if (at >= 0) damaged[at][0] = 0;
    xSemaphoreGive(busy);
    rescan();
    return ok;
}

// --- format ------------------------------------------------------------------------

bool storage_format(void)
{
    if (!mounted || !take_busy()) return false;
    esp_err_t err = esp_vfs_fat_sdcard_format(MOUNT, card);   // FAT32 (ESP-IDF has no exFAT)
    if (err == ESP_OK) mkdir(FOLDER, 0777);
    else ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
    memset(damaged, 0, sizeof damaged);
    xSemaphoreGive(busy);
    rescan();
    return err == ESP_OK;
}
