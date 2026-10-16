// boards/cyd28.c - the 2.8" resistive board's glass and touch chip (see cyd28.h, board.h).
#include "board.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cyd28";
#define Z_THRESHOLD 400                // pressure below this is nobody (the chip's Z1 + 4095 - Z2)
#define CAL_SPAN_MIN 800               // raw units between two crosses, else the taps were on one spot (the glass spans ~3000)

// --- the panel -----------------------------------------------------------------------------

static esp_lcd_panel_io_handle_t panel_io(int hz)
{
    esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = BOARD_LCD_DC, .cs_gpio_num = BOARD_LCD_CS, .pclk_hz = hz,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
    };
    esp_lcd_panel_io_handle_t h = NULL;
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &h);
    return h;
}

// Which chip drives the glass. The variant tables say ILI9341 on the one-USB
// boards and ST7789 on the two-USB revision. Both answer an ID register,
// slowly: the ILI9341 its RDID4 (0xD3: a dummy byte, 0x00, 0x93, 0x41), the
// ST7789 its RDDID (0x04: a dummy, 0x85, 0x85, 0x52). A chip that answers
// nothing is driven as an ST7789: that driver's init is the plain MIPI-DCS
// one and drew the first board here (one USB-C, 2026-10-15, all zeros back)
// cleanly, while the ILI9341 driver's vendor init made a mess of it. `screen
// panel` on the console settles a wrong guess either way.
static panel_t ask_panel(board_t *b)
{
    esp_lcd_panel_io_handle_t slow = panel_io(BOARD_LCD_READ_HZ);
    if (!slow) return PANEL_ILI9341;
    uint8_t id[4] = {0}, rd[4] = {0};
    esp_lcd_panel_io_rx_param(slow, 0xD3, id, sizeof id);
    esp_lcd_panel_io_rx_param(slow, 0x04, rd, sizeof rd);
    esp_lcd_panel_io_del(slow);
    memcpy(b->id, id, sizeof b->id);
    ESP_LOGI(TAG, "the panel answers D3: %02x %02x %02x %02x, 04: %02x %02x %02x %02x",
             id[0], id[1], id[2], id[3], rd[0], rd[1], rd[2], rd[3]);
    for (int i = 0; i < 3; i++) if (id[i] == 0x93 && id[i + 1] == 0x41) return PANEL_ILI9341;
    for (int i = 0; i < 2; i++) if (rd[i] == 0x85 && rd[i + 1] == 0x85 && rd[i + 2] == 0x52) return PANEL_ST7789;
    ESP_LOGW(TAG, "no panel ID read back: driven as an ST7789. `screen panel ili9341` if the glass says otherwise");
    return PANEL_ST7789;
}

bool board_glass_up(void)
{
    board_t *b = board();
    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCK, .mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = BOARD_LCD_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_DRAW_PIXELS * 2,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG, "SPI did not start"); return false; }
    b->panel_is = b->panel_set == PANEL_ILI9341 || b->panel_set == PANEL_ST7789 ? b->panel_set : ask_panel(b);
    // the color order follows the chip unless `screen bgr` said: the original one-USB board's ILI9341 glass is
    // BGR-wired (TFT_eSPI's ILI9341 default, which the CYD setup leaves alone), the two-USB ST7789 glass is RGB (2026-10-15)
    if (!b->bgr_kept) b->bgr = b->panel_is == PANEL_ILI9341;
    b->io = panel_io(BOARD_LCD_HZ);
    if (!b->io) return false;
    esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = -1, .bits_per_pixel = 16,
        .rgb_ele_order = b->bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
    };
    esp_err_t err = b->panel_is == PANEL_ILI9341 ? esp_lcd_new_panel_ili9341(b->io, &panel, &b->panel)
                                                 : esp_lcd_new_panel_st7789(b->io, &panel, &b->panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "no %s", board_panel_name()); return false; }
    return true;
}

const char *board_panel_name(void) { return board()->panel_is == PANEL_ILI9341 ? "ILI9341" : "ST7789"; }

// --- the touch chip, by hand ----------------------------------------------------------------
// One reading: 8 bits of a command out, then 16 clocks in (a busy bit, the 12
// bits of the reading, 3 of nothing). Mode 0: the chip takes DIN on the rising
// edge and changes DOUT on the falling one. ~2 us a half cycle: a reading in
// 0.1 ms, a whole touch (pressure, three X, three Y) in under a millisecond.
// The commands end in 01 (reference off, converter on, PENIRQ held) while a
// touch is read and in 00 for the last one, which powers the chip down and
// gives PENIRQ back.

static inline void tp_tick(void) { esp_rom_delay_us(2); }

static int tp_read(uint8_t cmd)
{
    unsigned in = 0;
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(BOARD_TP_MOSI, (cmd >> i) & 1);
        tp_tick();
        gpio_set_level(BOARD_TP_CLK, 1);
        tp_tick();
        gpio_set_level(BOARD_TP_CLK, 0);
    }
    gpio_set_level(BOARD_TP_MOSI, 0);
    for (int i = 0; i < 16; i++) {
        tp_tick();
        gpio_set_level(BOARD_TP_CLK, 1);
        in = in << 1 | (gpio_get_level(BOARD_TP_MISO) & 1);
        tp_tick();
        gpio_set_level(BOARD_TP_CLK, 0);
    }
    return (in >> 3) & 0x0FFF;
}

static int median3(int a, int c, int d)
{
    if (a > c) { int t = a; a = c; c = t; }
    if (c > d) { int t = c; c = d; d = t; }
    return a > c ? a : c;
}

bool board_touch_up(void)
{
    gpio_config_t out = {.pin_bit_mask = (1ULL << BOARD_TP_CLK) | (1ULL << BOARD_TP_MOSI) | (1ULL << BOARD_TP_CS), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_config_t in = {.pin_bit_mask = (1ULL << BOARD_TP_MISO) | (1ULL << BOARD_TP_IRQ), .mode = GPIO_MODE_INPUT};
    gpio_config(&in);                  // 36 and 39 have no pull-ups to give; the chip drives both
    gpio_set_level(BOARD_TP_CS, 1);
    gpio_set_level(BOARD_TP_CLK, 0);
    gpio_set_level(BOARD_TP_MOSI, 0);
    // is a chip there: untouched, its Z1 reads near 0 and Z2 near 4095. A floating
    // data line gives the same for both. Only a word for `show`: it is read either way
    bool answers = false;
    for (int i = 0; i < 5 && !answers; i++) {
        gpio_set_level(BOARD_TP_CS, 0);
        int z1 = tp_read(0xB1), z2 = tp_read(0xC1);
        tp_read(0x90);
        gpio_set_level(BOARD_TP_CS, 1);
        answers = z2 > z1 + 500;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return answers;
}

bool board_touch_raw(int *x, int *y, int *z)
{
    // PENIRQ is not consulted: it flickered high mid-press on the first board
    // (2026-10-15), and a flicker would be a release and a second tap. The
    // pressure reading is the proof and costs a fifth of a millisecond
    gpio_set_level(BOARD_TP_CS, 0);
    int z1 = tp_read(0xB1), z2 = tp_read(0xC1);
    int zz = z1 + 4095 - z2;
    int xs[3] = {0}, ys[3] = {0};
    if (zz >= Z_THRESHOLD) {
        for (int i = 0; i < 3; i++) { xs[i] = tp_read(0xD1); ys[i] = tp_read(0x91); }
    }
    tp_read(0x90);
    gpio_set_level(BOARD_TP_CS, 1);
    if (zz < Z_THRESHOLD) return false;
    if (x) *x = median3(xs[0], xs[1], xs[2]);
    if (y) *y = median3(ys[0], ys[1], ys[2]);
    if (z) *z = zz;
    return true;
}

// --- the calibration: raw -> the panel's native frame ------------------------------------------
// Two straight lines, one per native axis, from three crosses (top-left,
// top-right, bottom-right as drawn, whichever way the glass sits). From the
// first cross to the second only one native axis moved: the raw channel that
// moved more runs along it, the other channel along the other axis. Kept in
// flash in the native frame, so a calibration done in landscape serves
// portrait too.

typedef struct { int32_t r0, r1, n0, n1; } axis_t;   // raw r0 reads as native n0, r1 as n1
typedef struct { axis_t x, y; uint8_t swap; } cal_t;  // swap: native x comes from the chip's Y channel
static cal_t cal = {{200, 3800, 0, PANEL_W - 1}, {200, 3800, 0, PANEL_H - 1}, 0};   // a guess until `touch cal`
static bool cal_set;

static int along(const axis_t *a, int raw)
{
    return a->n0 + (int)((long)(raw - a->r0) * (a->n1 - a->n0) / (a->r1 - a->r0));
}

bool board_touch_native(int *x, int *y)
{
    int rx, ry;
    if (!board_touch_raw(&rx, &ry, NULL)) return false;
    *x = along(&cal.x, cal.swap ? ry : rx);
    *y = along(&cal.y, cal.swap ? rx : ry);
    return true;
}

bool board_touch_keep(const int raw[3][2], const int native[3][2])
{
    bool first_x = abs(native[1][0] - native[0][0]) > abs(native[1][1] - native[0][1]);   // the first move ran along native x
    bool first_rx = abs(raw[1][0] - raw[0][0]) > abs(raw[1][1] - raw[0][1]);              // and along the chip's X channel
    cal_t c = {.swap = first_x != first_rx};
    int cx = c.swap ? 1 : 0, cy = 1 - cx;
    int xa = first_x ? 0 : 1, xb = first_x ? 1 : 2;      // the pair of crosses apart in native x, and in native y
    int ya = first_x ? 1 : 0, yb = first_x ? 2 : 1;
    c.x = (axis_t){raw[xa][cx], raw[xb][cx], native[xa][0], native[xb][0]};
    c.y = (axis_t){raw[ya][cy], raw[yb][cy], native[ya][1], native[yb][1]};
    if (abs(c.x.r1 - c.x.r0) < CAL_SPAN_MIN || abs(c.y.r1 - c.y.r0) < CAL_SPAN_MIN) {   // kept, every tap would land on an edge for good
        ESP_LOGW(TAG, "calibration refused: raw %d..%d and %d..%d is too narrow", c.x.r0, c.x.r1, c.y.r0, c.y.r1);
        return false;
    }
    cal = c;
    cal_set = true;
    ESP_LOGI(TAG, "touch calibrated: native x from raw %d..%d (%d..%d), y from raw %d..%d (%d..%d), channels %s",
             c.x.r0, c.x.r1, c.x.n0, c.x.n1, c.y.r0, c.y.r1, c.y.n0, c.y.n1, c.swap ? "swapped" : "straight");
    return true;
}

bool board_touch_needs_cal(void) { return true; }
bool board_touch_calibrated(void) { return cal_set; }
const char *board_touch_words(void) { return cal_set ? "calibrated" : "NOT calibrated: touch cal"; }

void board_load_own(nvs_handle_t h)
{
    cal_t kept;
    size_t n = sizeof kept;
    if (nvs_get_blob(h, "cal2", &kept, &n) == ESP_OK && n == sizeof kept && kept.x.r0 != kept.x.r1 && kept.y.r0 != kept.y.r1) {
        cal = kept;
        cal_set = true;
    }
}

void board_save_own(nvs_handle_t h)
{
    if (cal_set) nvs_set_blob(h, "cal2", &cal, sizeof cal);
}
