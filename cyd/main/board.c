// board.c - see board.h. What every CYD shares: the words kept in flash, the
// backlight and the LED on one PWM timer, the light sensor, the BOOT button,
// and how the glass sits (rotation, and the touch mapped the same way). The
// glass and the touch chip themselves are in boards/<name>.c.
#include "board.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";
static board_t b;
static adc_oneshot_unit_handle_t adc;
static bool invert_kept;               // flash holds a word on invert; else the board's default

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

// --- what is kept in flash: the panel's word, the colors, how the glass sits, the board's own ---
// The keys `flip` and `mirror` of the programs before 0.5.0 are left alone:
// `rot` and `mir` carry the same meaning in the new scheme (the old mirror
// word would turn a landscape glass the wrong way).

static void load_prefs(void)
{
    b.bgr = BOARD_BGR_DEFAULT;
    b.invert = BOARD_INVERT_DEFAULT;
    nvs_handle_t h;
    if (nvs_open("pocket", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "panel", &v) == ESP_OK) b.panel_set = v <= PANEL_ST7796 ? v : PANEL_AUTO;
    if (nvs_get_u8(h, "invert", &v) == ESP_OK) { b.invert = v != 0; invert_kept = true; }
    if (nvs_get_u8(h, "bgr", &v) == ESP_OK) { b.bgr = v != 0; b.bgr_kept = true; }
    if (nvs_get_u8(h, "rot", &v) == ESP_OK && v < 4) b.rotation = v * 90;
    if (nvs_get_u8(h, "mir", &v) == ESP_OK) b.mirror = v != 0;
    board_load_own(h);
    nvs_close(h);
}

static void save_prefs(void)
{
    nvs_handle_t h;
    if (nvs_open("pocket", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "panel", b.panel_set);
    if (invert_kept) nvs_set_u8(h, "invert", b.invert);
    if (b.bgr_kept) nvs_set_u8(h, "bgr", b.bgr);
    nvs_set_u8(h, "rot", (b.rotation / 90) & 3);
    nvs_set_u8(h, "mir", b.mirror);
    board_save_own(h);
    nvs_commit(h);
    nvs_close(h);
}

// --- how the glass sits ----------------------------------------------------------------------
// The four ways, as the panel is told (MADCTL's MV, MX, MY in the ST77xx
// order: the axes exchanged first, then each mirrored), from the glass's own
// portrait. PANEL_MIRROR_X is a glass that scans its X the other way (the
// 3.5" does); `mirror` is the same correction in a person's hands. 90 and 270
// may trade places on a glass: the test card's TOP says, and this table is
// where they would be swapped.
static const turn_t TURNS[4] = {
    {true, true, false},               // 0: landscape, the reader's own way
    {false, false, false},             // 90: portrait
    {true, false, true},               // 180: landscape, the USB plug on the other side
    {false, true, true},               // 270: portrait, the other way up
};

turn_t board_turn(void)
{
    turn_t t = TURNS[(b.rotation / 90) & 3];
    t.mirror_x ^= (PANEL_MIRROR_X != 0) != b.mirror;
    return t;
}

int board_w(void) { return board_turn().swap ? PANEL_H : PANEL_W; }
int board_h(void) { return board_turn().swap ? PANEL_W : PANEL_H; }

void board_to_native(int x, int y, int *nx, int *ny)
{
    turn_t t = board_turn();
    int c = t.swap ? y : x, r = t.swap ? x : y;
    if (t.mirror_x) c = PANEL_W - 1 - c;
    if (t.mirror_y) r = PANEL_H - 1 - r;
    *nx = c; *ny = r;
}

void board_from_native(int nx, int ny, int *x, int *y)
{
    turn_t t = board_turn();
    if (t.mirror_x) nx = PANEL_W - 1 - nx;
    if (t.mirror_y) ny = PANEL_H - 1 - ny;
    *x = t.swap ? ny : nx;
    *y = t.swap ? nx : ny;
}

static void apply_orientation(void)
{
    turn_t t = board_turn();
    esp_lcd_panel_swap_xy(b.panel, t.swap);
    esp_lcd_panel_mirror(b.panel, t.mirror_x, t.mirror_y);
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

    // --- the glass, then the touch ---
    bool ok = board_glass_up();
    if (ok) {
        esp_lcd_panel_reset(b.panel);
        esp_lcd_panel_init(b.panel);
        esp_lcd_panel_invert_color(b.panel, b.invert);
        apply_orientation();
        esp_lcd_panel_disp_on_off(b.panel, true);
    }
    b.touch = board_touch_up();
    ESP_LOGI(TAG, "%s: %s %s, %d x %d at %d degrees, touch chip %s (%s), invert %d, bgr %d, mirror %d", BOARD_NAME,
             board_panel_name(), ok ? "up" : "MISSING", board_w(), board_h(), b.rotation, b.touch ? "answers" : "SILENT",
             board_touch_words(), b.invert, b.bgr, b.mirror);
    return ok;
}

board_t *board(void) { return &b; }

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

bool board_touch(int *x, int *y)
{
    int nx, ny, sx, sy;
    if (!board_touch_native(&nx, &ny)) return false;
    board_from_native(nx, ny, &sx, &sy);
    int w = board_w(), h = board_h();
    *x = sx < 0 ? 0 : sx >= w ? w - 1 : sx;
    *y = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
    return true;
}

bool board_touch_calibrate(const int raw[3][2], const int at[3][2])
{
    int native[3][2];
    for (int i = 0; i < 3; i++) board_to_native(at[i][0], at[i][1], &native[i][0], &native[i][1]);
    if (!board_touch_keep(raw, native)) return false;
    save_prefs();
    return true;
}

void board_set_invert(bool on)
{
    b.invert = on;
    invert_kept = true;
    if (b.panel) esp_lcd_panel_invert_color(b.panel, on);
    save_prefs();
}

void board_set_rotation(int degrees)
{
    b.rotation = ((degrees / 90) & 3) * 90;
    save_prefs();
}

void board_set_mirror(bool on)
{
    b.mirror = on;
    if (b.panel) apply_orientation();
    save_prefs();
}

void board_set_panel(panel_t which)
{
    b.panel_set = which;
    save_prefs();
}

void board_set_bgr(bool on)
{
    b.bgr = on;
    b.bgr_kept = true;
    save_prefs();
}
