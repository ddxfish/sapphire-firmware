// board.c - see board.h.
#include "board.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "board";
static board_t b;
static adc_oneshot_unit_handle_t adc;
static bool invert_kept;               // flash holds a word on invert; else the panel's kind decides

enum { CH_BL = 0, CH_R, CH_G, CH_B };
#define LEDC_BITS LEDC_TIMER_8_BIT
#define Z_THRESHOLD 400                // pressure below this is nobody (the chip's Z1 + 4095 - Z2)
#define CAL_SPAN_MIN 800               // raw units between two crosses, else the taps were on one spot (the glass spans ~3000)

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

// --- what is kept in flash: the panel's word, the colors, how the glass sits, the calibration ---

typedef struct { int32_t cal[4]; uint8_t swap, flip, mirror; } cal_blob_t;

static void load_prefs(void)
{
    b.cal[0] = 200; b.cal[1] = 3800; b.cal[2] = 200; b.cal[3] = 3800;   // a guess until `touch cal`
    b.cal_swap = true;
    nvs_handle_t h;
    if (nvs_open("pocket", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "panel", &v) == ESP_OK) b.panel_set = v <= PANEL_ST7789 ? v : PANEL_AUTO;
    if (nvs_get_u8(h, "invert", &v) == ESP_OK) { b.invert = v != 0; invert_kept = true; }
    if (nvs_get_u8(h, "flip", &v) == ESP_OK) b.flip = v != 0;
    if (nvs_get_u8(h, "bgr", &v) == ESP_OK) b.bgr = v != 0;
    if (nvs_get_u8(h, "mirror", &v) == ESP_OK) b.mirror = v != 0;
    cal_blob_t cal;
    size_t n = sizeof cal;
    if (nvs_get_blob(h, "cal", &cal, &n) == ESP_OK && n == sizeof cal) {
        for (int i = 0; i < 4; i++) b.cal[i] = cal.cal[i];
        b.cal_swap = cal.swap; b.cal_flip = cal.flip; b.cal_mirror = cal.mirror;
        b.cal_set = true;
    }
    nvs_close(h);
}

static void save_prefs(void)
{
    nvs_handle_t h;
    if (nvs_open("pocket", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "panel", b.panel_set);
    if (invert_kept) nvs_set_u8(h, "invert", b.invert);
    nvs_set_u8(h, "flip", b.flip);
    nvs_set_u8(h, "bgr", b.bgr);
    nvs_set_u8(h, "mirror", b.mirror);
    if (b.cal_set) {
        cal_blob_t cal = {.swap = b.cal_swap, .flip = b.cal_flip, .mirror = b.cal_mirror};
        for (int i = 0; i < 4; i++) cal.cal[i] = b.cal[i];
        nvs_set_blob(h, "cal", &cal, sizeof cal);
    }
    nvs_commit(h);
    nvs_close(h);
}

// --- the panel -----------------------------------------------------------------------------

static void apply_orientation(void)
{
    // landscape: the panel's axes swapped. flip = 180 degrees = both axes the
    // other way; mirror = the X axis alone. Between them, every way it can sit.
    esp_lcd_panel_swap_xy(b.panel, true);
    esp_lcd_panel_mirror(b.panel, b.flip != b.mirror, b.flip);
}

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
static panel_t ask_panel(void)
{
    esp_lcd_panel_io_handle_t slow = panel_io(BOARD_LCD_READ_HZ);
    if (!slow) return PANEL_ILI9341;
    uint8_t id[4] = {0}, rd[4] = {0};
    esp_lcd_panel_io_rx_param(slow, 0xD3, id, sizeof id);
    esp_lcd_panel_io_rx_param(slow, 0x04, rd, sizeof rd);
    esp_lcd_panel_io_del(slow);
    memcpy(b.id, id, sizeof b.id);
    ESP_LOGI(TAG, "the panel answers D3: %02x %02x %02x %02x, 04: %02x %02x %02x %02x",
             id[0], id[1], id[2], id[3], rd[0], rd[1], rd[2], rd[3]);
    for (int i = 0; i < 3; i++) if (id[i] == 0x93 && id[i + 1] == 0x41) return PANEL_ILI9341;
    for (int i = 0; i < 2; i++) if (rd[i] == 0x85 && rd[i + 1] == 0x85 && rd[i + 2] == 0x52) return PANEL_ST7789;
    ESP_LOGW(TAG, "no panel ID read back: driven as an ST7789. `screen panel ili9341` if the glass says otherwise");
    return PANEL_ST7789;
}

static bool panel_up(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_SCK, .mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = BOARD_LCD_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_W * BOARD_DRAW_LINES * 2,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG, "SPI did not start"); return false; }
    b.panel_is = b.panel_set != PANEL_AUTO ? b.panel_set : ask_panel();
    if (!invert_kept) b.invert = false;        // the first board wanted none, whatever its chip; `screen invert 1` for one that does
    b.io = panel_io(BOARD_LCD_HZ);
    if (!b.io) return false;
    esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = -1, .bits_per_pixel = 16,
        .rgb_ele_order = b.bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,   // the first board: red first (blue drew orange as BGR)
    };
    esp_err_t err = b.panel_is == PANEL_ILI9341 ? esp_lcd_new_panel_ili9341(b.io, &panel, &b.panel)
                                                : esp_lcd_new_panel_st7789(b.io, &panel, &b.panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "no %s", board_panel_name()); return false; }
    esp_lcd_panel_reset(b.panel);
    esp_lcd_panel_init(b.panel);
    esp_lcd_panel_invert_color(b.panel, b.invert);
    apply_orientation();
    esp_lcd_panel_disp_on_off(b.panel, true);
    return true;
}

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

static void touch_pins(void)
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
    for (int i = 0; i < 5 && !b.touch; i++) {
        gpio_set_level(BOARD_TP_CS, 0);
        int z1 = tp_read(0xB1), z2 = tp_read(0xC1);
        tp_read(0x90);
        gpio_set_level(BOARD_TP_CS, 1);
        b.touch = z2 > z1 + 500;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
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

void board_touch_probe(int *irq, int *z1, int *z2, int *x, int *y)
{
    *irq = gpio_get_level(BOARD_TP_IRQ);
    gpio_set_level(BOARD_TP_CS, 0);
    *z1 = tp_read(0xB1);
    *z2 = tp_read(0xC1);
    *x = tp_read(0xD1);
    *y = tp_read(0x91);
    tp_read(0x90);
    gpio_set_level(BOARD_TP_CS, 1);
}

bool board_touch(int *x, int *y)
{
    int rx, ry;
    if (!board_touch_raw(&rx, &ry, NULL)) return false;
    int across = b.cal_swap ? ry : rx, down = b.cal_swap ? rx : ry;
    int dx = b.cal[1] - b.cal[0], dy = b.cal[3] - b.cal[2];
    if (!dx || !dy) return false;
    int sx = CAL_INSET + (across - b.cal[0]) * (BOARD_W - 2 * CAL_INSET) / dx;
    int sy = CAL_INSET + (down - b.cal[2]) * (BOARD_H - 2 * CAL_INSET) / dy;
    if (b.flip != b.cal_flip) { sx = BOARD_W - 1 - sx; sy = BOARD_H - 1 - sy; }   // the glass sits differently now
    if (b.mirror != b.cal_mirror) sx = BOARD_W - 1 - sx;
    *x = sx < 0 ? 0 : sx >= BOARD_W ? BOARD_W - 1 : sx;
    *y = sy < 0 ? 0 : sy >= BOARD_H ? BOARD_H - 1 : sy;
    return true;
}

bool board_touch_calibrate(const int raw[3][2])
{
    // three crosses: top-left, top-right, bottom-right. From the first to the
    // second only the screen's X moved: the channel that moved more runs across
    int da = abs(raw[1][0] - raw[0][0]), db = abs(raw[1][1] - raw[0][1]);
    bool swap = db > da;
    int ax = swap ? 1 : 0, ay = 1 - ax;
    int cal[4] = {raw[0][ax], raw[1][ax], raw[1][ay], raw[2][ay]};
    if (abs(cal[1] - cal[0]) < CAL_SPAN_MIN || abs(cal[3] - cal[2]) < CAL_SPAN_MIN) {   // kept, every tap would land on an edge for good
        ESP_LOGW(TAG, "calibration refused: across %d..%d, down %d..%d is too narrow", cal[0], cal[1], cal[2], cal[3]);
        return false;
    }
    b.cal_swap = swap;
    for (int i = 0; i < 4; i++) b.cal[i] = cal[i];
    b.cal_flip = b.flip; b.cal_mirror = b.mirror;
    b.cal_set = true;
    save_prefs();
    ESP_LOGI(TAG, "touch calibrated: across %d..%d, down %d..%d, %s", b.cal[0], b.cal[1], b.cal[2], b.cal[3],
             b.cal_swap ? "channels swapped" : "channels straight");
    return true;
}

// --- the board ---------------------------------------------------------------------------------

bool board_init(void)
{
    load_prefs();

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

    bool ok = panel_up();
    touch_pins();
    ESP_LOGI(TAG, "%s: %s %s, touch chip %s, invert %d, bgr %d, flip %d, mirror %d, calibration %s", BOARD_NAME,
             board_panel_name(), ok ? "up" : "MISSING", b.touch ? "answers" : "SILENT", b.invert, b.bgr, b.flip, b.mirror,
             b.cal_set ? "kept" : "a guess (run `touch cal`)");
    return ok;
}

board_t *board(void) { return &b; }

const char *board_panel_name(void) { return b.panel_is == PANEL_ILI9341 ? "ILI9341" : "ST7789"; }

void board_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    pwm_set(CH_BL, percent * 256 / 100);   // 8-bit LEDC counts to 256: that is the pin high the whole period
}

void board_led(uint8_t r, uint8_t g, uint8_t b_)
{
    pwm_set(CH_R, 256 - r);            // active low: duty 256 = high the whole period = off (255 left it lit 1/256 of the time)
    pwm_set(CH_G, 256 - g);
    pwm_set(CH_B, 256 - b_);
}

int board_light_raw(void)
{
    int raw = -1;
    if (!adc) return -1;
    for (int i = 0; i < 3; i++) {                           // two readers at once (/health and the Status page): one gets a timeout, so once more
        if (adc_oneshot_read(adc, ADC_CHANNEL_6, &raw) == ESP_OK) return raw;
        vTaskDelay(1);
    }
    return -1;
}

bool board_button(void) { return gpio_get_level(BOARD_BOOT_BUTTON) == 0; }

void board_set_invert(bool on)
{
    b.invert = on;
    invert_kept = true;
    if (b.panel) esp_lcd_panel_invert_color(b.panel, on);
    save_prefs();
}

void board_set_flip(bool on)
{
    b.flip = on;
    if (b.panel) apply_orientation();
    save_prefs();
}

void board_set_mirror(bool on)
{
    b.mirror = on;
    if (b.panel) apply_orientation();
    save_prefs();
}

void board_set_bgr(bool on)
{
    b.bgr = on;
    save_prefs();
}

void board_set_panel(panel_t which)
{
    b.panel_set = which;
    save_prefs();
}
