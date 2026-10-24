// storage.c - see storage.h. The microSD over a plain SPI bus, FATFS, mounted
// at /sdcard. Lean on purpose: mount + how much room. Apps read and write plain
// files under the mount; the backup protocol lives with the Backup app later.
#include "storage.h"
#include "board.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

#define SD_HOST SPI2_HOST            // the screen owns SPI3_HOST

static sdmmc_card_t *card;
static bool mounted;

bool storage_start(void)
{
    if (mounted) return true;
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_SD_MOSI, .miso_io_num = BOARD_SD_MISO, .sclk_io_num = BOARD_SD_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = already up, fine
        ESP_LOGW(TAG, "SD SPI bus did not start: %s", esp_err_to_name(err));
        return false;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = BOARD_SD_CS;
    slot.host_id = SD_HOST;
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,                   // never wipe a person's card on our own
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount(STORAGE_MOUNT, &host, &slot, &cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no card mounted (%s): the disk is off until one is in and FAT32", esp_err_to_name(err));
        return false;
    }
    mounted = true;
    uint64_t total = 0, freeb = 0;
    storage_room(&freeb, &total);
    ESP_LOGI(TAG, "card %s: %llu MB, %llu MB free at %s", card->cid.name,
             (unsigned long long)(total >> 20), (unsigned long long)(freeb >> 20), STORAGE_MOUNT);
    return true;
}

void storage_stop(void)
{
    if (!mounted) return;
    esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT, card);
    card = NULL;
    mounted = false;
    spi_bus_free(SD_HOST);                                 // give the ~37 KB back
}

bool storage_ok(void) { return mounted; }

bool storage_room(uint64_t *free_bytes, uint64_t *total_bytes)
{
    if (!mounted) return false;
    uint64_t t = 0, f = 0;
    if (esp_vfs_fat_info(STORAGE_MOUNT, &t, &f) != ESP_OK) return false;
    if (free_bytes) *free_bytes = f;
    if (total_bytes) *total_bytes = t;
    return true;
}

const char *storage_card_name(void) { return mounted && card ? card->cid.name : ""; }
