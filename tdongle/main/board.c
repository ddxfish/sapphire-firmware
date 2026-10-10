// board.c - see board.h.
#include "board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "board";
static board_t b;
#define LED_GLOBAL 6                   // the APA102's own 0..31 dimmer: a bare LED at eye height

// ST7735S, 0.96" IPS: the sequence the gadget firmware ran on this stick
static const struct { uint8_t cmd; uint8_t data[16]; uint8_t len; uint8_t wait_ms; } INIT[] = {
    {0x01, {0}, 0, 150}, {0x11, {0}, 0, 255},
    {0xB1, {0x01, 0x2C, 0x2D}, 3, 0}, {0xB2, {0x01, 0x2C, 0x2D}, 3, 0},
    {0xB3, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}, 6, 0}, {0xB4, {0x07}, 1, 0},
    {0xC0, {0xA2, 0x02, 0x84}, 3, 0}, {0xC1, {0xC5}, 1, 0}, {0xC2, {0x0A, 0x00}, 2, 0},
    {0xC3, {0x8A, 0x2A}, 2, 0}, {0xC4, {0x8A, 0xEE}, 2, 0}, {0xC5, {0x0E}, 1, 0},
    {0x21, {0}, 0, 0},                 // colors inverted: the panel is IPS
    {0x3A, {0x05}, 1, 0},              // 16 bits a pixel
    {0xE0, {0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10}, 16, 0},
    {0xE1, {0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10}, 16, 0},
    {0x13, {0}, 0, 10}, {0x29, {0}, 0, 100},
};

static void load_prefs(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("pocket", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_u8(h, "flip", &v) == ESP_OK) b.flip = v != 0;
    nvs_close(h);
}

bool board_init(void)
{
    load_prefs();

    // --- the backlight: PWM, on when low ---
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = BOARD_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0, .duty = 255, .hpoint = 0,
    };
    ledc_channel_config(&c);

    // --- the LED, the button, the panel's reset ---
    gpio_config_t out = {.pin_bit_mask = (1ULL << BOARD_LED_DATA) | (1ULL << BOARD_LED_CLOCK) | (1ULL << BOARD_LCD_RST), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    board_led(0, 0, 0);
    gpio_config_t btn = {.pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
    gpio_config(&btn);

    // --- the panel: an ST7735S. ESP-IDF's ST7789 object draws on it (the two
    // share every command it uses after the start), but the start-up sequence
    // is the ST7735S's own, sent here instead of esp_lcd_panel_init ---
    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCK, .mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_W * BOARD_DRAW_LINES * 2,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG, "SPI did not start"); return false; }
    esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = BOARD_LCD_DC, .cs_gpio_num = BOARD_LCD_CS, .pclk_hz = BOARD_LCD_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &b.io) != ESP_OK) return false;
    esp_lcd_panel_dev_config_t panel = {.reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16};
    if (esp_lcd_new_panel_st7789(b.io, &panel, &b.panel) != ESP_OK) { ESP_LOGE(TAG, "no panel object"); return false; }
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    for (size_t i = 0; i < sizeof INIT / sizeof INIT[0]; i++) {
        esp_lcd_panel_io_tx_param(b.io, INIT[i].cmd, INIT[i].len ? INIT[i].data : NULL, INIT[i].len);
        if (INIT[i].wait_ms) vTaskDelay(pdMS_TO_TICKS(INIT[i].wait_ms));
    }
    esp_lcd_panel_set_gap(b.panel, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y);
    ESP_LOGI(TAG, "%s: panel up, flip %d", BOARD_NAME, b.flip);
    return true;
}

board_t *board(void) { return &b; }

void board_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 255 - percent * 255 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// One APA102, bit-banged: a start frame, the LED (its dimmer, then blue, green, red), an end frame.
void board_led(uint8_t r, uint8_t g, uint8_t b_)
{
    const uint8_t frame[12] = {0, 0, 0, 0, 0xE0 | LED_GLOBAL, b_, g, r, 0xFF, 0xFF, 0xFF, 0xFF};
    for (size_t i = 0; i < sizeof frame; i++)
        for (int bit = 7; bit >= 0; bit--) {
            gpio_set_level(BOARD_LED_DATA, (frame[i] >> bit) & 1);
            gpio_set_level(BOARD_LED_CLOCK, 1);
            gpio_set_level(BOARD_LED_CLOCK, 0);
        }
}

bool board_button(void) { return gpio_get_level(BOARD_BOOT_BUTTON) == 0; }

void board_set_flip(bool on)
{
    b.flip = on;
    nvs_handle_t h;
    if (nvs_open("pocket", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "flip", on);
    nvs_commit(h);
    nvs_close(h);
}

bool board_sd_select(bool on)
{
    gpio_config_t d3 = {.pin_bit_mask = 1ULL << BOARD_SD_D3, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&d3);
    gpio_set_level(BOARD_SD_D3, on);
    return true;
}
