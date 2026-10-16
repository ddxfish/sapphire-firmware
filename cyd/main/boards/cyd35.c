// boards/cyd35.c - the 3.5" capacitive board's glass and touch chip (see cyd35.h, board.h).
#include "board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cyd35";
static esp_lcd_touch_handle_t touch;

// --- the panel -----------------------------------------------------------------------------

bool board_glass_up(void)
{
    board_t *b = board();
    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCK, .mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = BOARD_LCD_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_DRAW_PIXELS * 2,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG, "SPI did not start"); return false; }
    esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = BOARD_LCD_DC, .cs_gpio_num = BOARD_LCD_CS, .pclk_hz = BOARD_LCD_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &b->io) != ESP_OK) return false;
    esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = -1, .bits_per_pixel = 16,
        .rgb_ele_order = b->bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
    };
    b->panel_is = PANEL_ST7796;        // the only chip this glass comes with; `screen panel` has no say
    if (esp_lcd_new_panel_st7796(b->io, &panel, &b->panel) != ESP_OK) { ESP_LOGE(TAG, "no ST7796"); return false; }
    return true;
}

const char *board_panel_name(void) { return "ST7796"; }

// --- the touch chip: a GT911 at 0x5D, polled ---------------------------------------------------
// Found with the console's `i2c 33 32` scan (2026-10-06): the board sold as
// a CST820 one answers at the GT911's address, and its config says the
// panel is 320 x 480. Its INT pin is soldered to ground by a routing
// mistake, so: (1) the reset below holds the INT GPIO low for form's sake
// (low into ground is no harm) and the chip sits at 0x5D either way; (2)
// the driver is given NO interrupt pin, and nobody waits for one. It is
// polled, by board_touch_raw.

bool board_touch_up(void)
{
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
    if (i2c_new_master_bus(&i2c, &i2c_bus) != ESP_OK) { ESP_LOGW(TAG, "I2C did not start: no touch"); return false; }
    esp_lcd_panel_io_i2c_config_t tp_io = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t tp_io_h = NULL;
    esp_lcd_touch_config_t tp = {
        .x_max = PANEL_W, .y_max = PANEL_H,
        .rst_gpio_num = -1, .int_gpio_num = -1,      // reset done above; no interrupt line: polled
        .levels = {.reset = 0, .interrupt = 0},
    };
    if (esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io, &tp_io_h) != ESP_OK
        || esp_lcd_touch_new_i2c_gt911(tp_io_h, &tp, &touch) != ESP_OK) {
        ESP_LOGW(TAG, "no touch chip answered on I2C (SDA %d SCL %d)", BOARD_TP_SDA, BOARD_TP_SCL);
        touch = NULL;
        return false;
    }
    // the touch panel's own idea of its size (GT911 config 0x8048: X max, 0x804A: Y max,
    // little-endian) is the screen's size: a touch panel is glued to one screen
    uint8_t r[4] = {0};
    if (esp_lcd_panel_io_rx_param(tp_io_h, 0x8048, r, sizeof r) == ESP_OK)
        ESP_LOGI(TAG, "touch panel says %u x %u", r[0] | r[1] << 8, r[2] | r[3] << 8);
    return true;
}

bool board_touch_raw(int *x, int *y, int *z)
{
    if (!touch) return false;
    esp_lcd_touch_read_data(touch);
    uint16_t tx[1], ty[1];
    uint8_t n = 0;
    if (!esp_lcd_touch_get_coordinates(touch, tx, ty, NULL, &n, 1) || !n) return false;
    if (x) *x = tx[0];
    if (y) *y = ty[0];
    if (z) *z = 1;
    return true;
}

// The chip reports in the glass's own portrait as a person sees it, which on
// this glass is the panel's frame with its X already turned the other way
// (PANEL_MIRROR_X: the program before 0.5.0 mirrored the panel and left the
// touch alone, and they agreed). Back to the panel's frame here.
bool board_touch_native(int *x, int *y)
{
    int gx, gy;
    if (!board_touch_raw(&gx, &gy, NULL)) return false;
    *x = PANEL_MIRROR_X ? PANEL_W - 1 - gx : gx;
    *y = gy;
    return true;
}

bool board_touch_keep(const int raw[3][2], const int native[3][2]) { return false; }   // a capacitive glass: nothing to calibrate
bool board_touch_needs_cal(void) { return false; }
bool board_touch_calibrated(void) { return true; }
const char *board_touch_words(void) { return "capacitive"; }
void board_load_own(nvs_handle_t h) {}
void board_save_own(nvs_handle_t h) {}
