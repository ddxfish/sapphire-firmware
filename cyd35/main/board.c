// board.c - see board.h.
#include "board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "board";
static board_t b;
static adc_oneshot_unit_handle_t adc;

enum { CH_BL = 0, CH_R, CH_G, CH_B };
#define LEDC_BITS LEDC_TIMER_8_BIT

static void pwm_pin(int channel, int gpio)
{
    ledc_channel_config_t c = {
        .gpio_num = gpio, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = channel,
        .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0,
    };
    ledc_channel_config(&c);
}

static void pwm_set(int channel, int duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

static void load_screen_prefs(void)
{
    b.invert = false;                  // seen as CMYK with it on (2026-10-06); `screen invert 1` turns it on
    b.flip = false;
    nvs_handle_t h;
    if (nvs_open("pocket", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "invert", &v) == ESP_OK) b.invert = v != 0;
    if (nvs_get_u8(h, "flip", &v) == ESP_OK) b.flip = v != 0;
    nvs_close(h);
}

static void save_screen_prefs(void)
{
    nvs_handle_t h;
    if (nvs_open("pocket", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "invert", b.invert);
    nvs_set_u8(h, "flip", b.flip);
    nvs_commit(h);
    nvs_close(h);
}

static void apply_orientation(void)
{
    // the panel is mounted portrait, the plug at the bottom, and its X axis
    // runs the other way (a mirror image with no mirroring, 2026-10-06).
    // flip = 180 degrees = the other mirror of each axis.
    esp_lcd_panel_swap_xy(b.panel, false);
    esp_lcd_panel_mirror(b.panel, !b.flip, b.flip);
    if (b.touch) {
        esp_lcd_touch_set_swap_xy(b.touch, false);
        esp_lcd_touch_set_mirror_x(b.touch, b.flip);
        esp_lcd_touch_set_mirror_y(b.touch, b.flip);
    }
}

bool board_init(void)
{
    load_screen_prefs();

    // --- the light, the LED and the backlight, one 5 kHz timer ---
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_BITS,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    pwm_pin(CH_BL, BOARD_LCD_BL);
    pwm_pin(CH_R, BOARD_LED_R);
    pwm_pin(CH_G, BOARD_LED_G);
    pwm_pin(CH_B, BOARD_LED_B);
    board_led(0, 0, 0);

    // --- the button and the light sensor ---
    gpio_config_t btn = {.pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
    gpio_config(&btn);
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit, &adc) == ESP_OK) {
        adc_oneshot_chan_cfg_t ch = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
        adc_oneshot_config_channel(adc, ADC_CHANNEL_6, &ch);
    }

    // --- the panel ---
    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCK, .mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = BOARD_LCD_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_W * BOARD_DRAW_LINES * 2,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG, "SPI did not start"); return false; }
    esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = BOARD_LCD_DC, .cs_gpio_num = BOARD_LCD_CS, .pclk_hz = BOARD_LCD_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &b.io) != ESP_OK) return false;
    esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16,   // BGR: red showed as yellow with RGB + invert (2026-10-06)
    };
    if (esp_lcd_new_panel_st7796(b.io, &panel, &b.panel) != ESP_OK) { ESP_LOGE(TAG, "no ST7796"); return false; }
    esp_lcd_panel_reset(b.panel);
    esp_lcd_panel_init(b.panel);
    esp_lcd_panel_invert_color(b.panel, b.invert);
    esp_lcd_panel_disp_on_off(b.panel, true);

    // --- the touch chip: a GT911 at 0x5D, polled ---
    // Found with the console's `i2c 33 32` scan (2026-10-06): the board sold as
    // a CST820 one answers at the GT911's address, and its config says the
    // panel is 320 x 480. On this board (ESP32-3248S035C) the chip's INT pin is
    // soldered to ground by a routing mistake, so: (1) the reset below holds
    // our INT GPIO low for form's sake and the chip sits at 0x5D either way;
    // (2) the driver is given NO interrupt pin, because esp_lvgl_port would
    // then read the chip only when that interrupt fires, which is never
    // (zero taps for an hour, 2026-10-06). It is polled.
    gpio_config_t tp_pins = {.pin_bit_mask = (1ULL << BOARD_TP_RST) | (1ULL << BOARD_TP_INT), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&tp_pins);
    gpio_set_level(BOARD_TP_INT, 0);
    gpio_set_level(BOARD_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(60));
    gpio_reset_pin(BOARD_TP_INT);      // back to an input: the chip's line, not ours
    i2c_master_bus_config_t i2c = {
        .i2c_port = I2C_NUM_0, .sda_io_num = BOARD_TP_SDA, .scl_io_num = BOARD_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    if (i2c_new_master_bus(&i2c, &i2c_bus) == ESP_OK) {
        esp_lcd_panel_io_i2c_config_t tp_io = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        tp_io.scl_speed_hz = 400000;
        esp_lcd_panel_io_handle_t tp_io_h = NULL;
        esp_lcd_touch_config_t tp = {
            .x_max = BOARD_W, .y_max = BOARD_H,
            .rst_gpio_num = -1, .int_gpio_num = -1,      // reset done above; no interrupt line: polled
            .levels = {.reset = 0, .interrupt = 0},
        };
        if (esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io, &tp_io_h) != ESP_OK
            || esp_lcd_touch_new_i2c_gt911(tp_io_h, &tp, &b.touch) != ESP_OK) {
            ESP_LOGW(TAG, "no touch chip answered on I2C (SDA %d SCL %d)", BOARD_TP_SDA, BOARD_TP_SCL);
            b.touch = NULL;
        } else {
            b.touch_io = tp_io_h;
            // the touch panel's own idea of its size (GT911 config 0x8048: X max, 0x804A: Y max,
            // little-endian) is the screen's size: a touch panel is glued to one screen
            uint8_t r[4] = {0};
            if (esp_lcd_panel_io_rx_param(tp_io_h, 0x8048, r, sizeof r) == ESP_OK)
                ESP_LOGI(TAG, "touch panel says %u x %u", r[0] | r[1] << 8, r[2] | r[3] << 8);
        }
    } else {
        ESP_LOGW(TAG, "I2C did not start: no touch");
    }
    apply_orientation();
    ESP_LOGI(TAG, "%s: panel up, touch %s, invert %d, flip %d", BOARD_NAME, b.touch ? "up" : "MISSING", b.invert, b.flip);
    return true;
}

board_t *board(void) { return &b; }

void board_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    pwm_set(CH_BL, percent * 255 / 100);
}

void board_led(uint8_t r, uint8_t g, uint8_t b_)
{
    pwm_set(CH_R, 255 - r);            // active low: full duty = off
    pwm_set(CH_G, 255 - g);
    pwm_set(CH_B, 255 - b_);
}

int board_light_raw(void)
{
    int raw = -1;
    if (!adc || adc_oneshot_read(adc, ADC_CHANNEL_6, &raw) != ESP_OK) return -1;
    return raw;
}

bool board_button(void) { return gpio_get_level(BOARD_BOOT_BUTTON) == 0; }

void board_set_invert(bool on)
{
    b.invert = on;
    if (b.panel) esp_lcd_panel_invert_color(b.panel, on);
    save_screen_prefs();
}

void board_set_flip(bool on)
{
    b.flip = on;
    if (b.panel) apply_orientation();
    save_screen_prefs();
}
