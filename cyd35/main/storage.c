// storage.c - see storage.h. The card over SDMMC (1-bit, GPIO matrix) or,
// when board.h says BOARD_SD_SPI, over a plain SPI bus; FATFS either way.
// The same file serves every board: only the mount differs (2026-10-06).
#include "storage.h"
#include "board.h"                     // first: it says how the card is wired

#include <dirent.h>
#include <errno.h>
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

static sdmmc_card_t *card;
static bool mounted;
static SemaphoreHandle_t busy;         // one write or one format at a time

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
    if (xSemaphoreTake(busy, 0) != pdTRUE) return true;
    xSemaphoreGive(busy);
    return false;
}

storage_write_t *storage_write_begin(const char *name, uint64_t expect)
{
    if (!mounted || !storage_name_ok(name)) return NULL;
    if (xSemaphoreTake(busy, 0) != pdTRUE) return NULL;
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

static void close_free(storage_write_t *w)
{
    if (w->f) fclose(w->f);
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
    if (ok) {
        unlink(final);                                     // FAT rename does not replace
        ok = rename(w->part, final) == 0;
    }
    if (!ok) unlink(w->part);
    if (size) *size = w->got;
    close_free(w);
    return ok;
}

void storage_write_drop(storage_write_t *w)
{
    if (w->f) { fclose(w->f); w->f = NULL; }
    unlink(w->part);
    close_free(w);
}

// --- format ------------------------------------------------------------------------

bool storage_format(void)
{
    if (!mounted || xSemaphoreTake(busy, 0) != pdTRUE) return false;
    esp_err_t err = esp_vfs_fat_sdcard_format(MOUNT, card);   // FAT32 (ESP-IDF has no exFAT)
    if (err == ESP_OK) mkdir(FOLDER, 0777);
    else ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
    xSemaphoreGive(busy);
    return err == ESP_OK;
}
