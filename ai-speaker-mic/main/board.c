// board.c - see board.h. The I2S shape (TX standard stereo, RX TDM with four
// slots, one clock for both) is the one Xiaozhi proved on this board.
#include "board.h"
#include "settings.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_cst816s.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_codec_dev_defaults.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";
static i2c_master_bus_handle_t i2c;
static esp_io_expander_handle_t expander;
static i2s_chan_handle_t tx, rx;
static board_audio_t audio;
static board_glass_t glass;
static const char *trouble = "";       // why board_init stopped, in words, for /health
static int touch_errors, touch_bad, bus_resets, bus_held;   // bus_held: clock pulses it took to free SDA at boot, 0 = it was free
static int64_t touch_rest_until;
#define TOUCH_TIMEOUT_MS 30            // one poll of the touch chip; the library's 0 means forever (the 2026-10-08 freeze)
#define TOUCH_BAD_ROW 30               // this many failed polls in a row (a second): the poll rests a second

#define PIN_PA IO_EXPANDER_PIN_NUM_8
#define PIN_LCD_RESET IO_EXPANDER_PIN_NUM_0
#define PIN_TOUCH_RESET IO_EXPANDER_PIN_NUM_1
#define PIN_CAMERA_DOWN IO_EXPANDER_PIN_NUM_5
#define PIN_CAMERA_SEL IO_EXPANDER_PIN_NUM_6
#define PIN_SD_CS IO_EXPANDER_PIN_NUM_3          // the card's D3/CS (board.h)

static bool expander_init(void)
{
    // Left in its power-up state the expander lets the amplifier float, and
    // the board is dead quiet. This is Xiaozhi's proven sequence.
    if (esp_io_expander_new_i2c_tca95xx_16bit(i2c, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &expander) != ESP_OK) {
        expander = NULL;
        return false;
    }
    uint32_t outputs = PIN_LCD_RESET | PIN_TOUCH_RESET | PIN_CAMERA_DOWN | PIN_CAMERA_SEL | PIN_PA | PIN_SD_CS;
    if (esp_io_expander_set_dir(expander, outputs, IO_EXPANDER_OUTPUT) != ESP_OK) {
        esp_io_expander_del(expander);
        expander = NULL;
        return false;
    }
    esp_io_expander_set_level(expander, PIN_SD_CS, 1);           // SD mode, not SPI mode
    esp_io_expander_set_level(expander, PIN_LCD_RESET | PIN_TOUCH_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_io_expander_set_level(expander, PIN_LCD_RESET | PIN_TOUCH_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_io_expander_set_level(expander, PIN_LCD_RESET | PIN_TOUCH_RESET, 1);
    esp_io_expander_set_level(expander, PIN_CAMERA_DOWN, 1);      // the camera stays powered DOWN until a shot (camera.c); Xiaozhi left it on
    esp_io_expander_set_level(expander, PIN_CAMERA_SEL, 1);        // its clocks on GPIO 43/44, never 19/20 (USB)
    esp_io_expander_set_level(expander, PIN_PA, 1);
    return true;
}

static bool i2s_init(int rate)
{
    i2s_chan_config_t chan = {
        .id = I2S_NUM_0, .role = I2S_ROLE_MASTER, .dma_desc_num = 6, .dma_frame_num = 240,
        .auto_clear_after_cb = true,
    };
    if (i2s_new_channel(&chan, &tx, &rx) != ESP_OK) return false;
    i2s_std_config_t std = {
        .clk_cfg = {.sample_rate_hz = rate, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = GPIO_NUM_12, .bclk = GPIO_NUM_13, .ws = GPIO_NUM_14,
                     .dout = GPIO_NUM_16, .din = I2S_GPIO_UNUSED},
    };
    i2s_tdm_config_t tdm = {
        .clk_cfg = {.sample_rate_hz = rate, .clk_src = I2S_CLK_SRC_DEFAULT,
                    .mclk_multiple = I2S_MCLK_MULTIPLE_256, .bclk_div = 8},
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {.mclk = GPIO_NUM_12, .bclk = GPIO_NUM_13, .ws = GPIO_NUM_14,
                     .dout = I2S_GPIO_UNUSED, .din = GPIO_NUM_15},
    };
    return i2s_channel_init_std_mode(tx, &std) == ESP_OK && i2s_channel_init_tdm_mode(rx, &tdm) == ESP_OK
        && i2s_channel_enable(tx) == ESP_OK && i2s_channel_enable(rx) == ESP_OK;
}

static bool codecs_init(int rate)
{
    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_cfg = {.port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c};
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!data_if || !gpio_if || !out_ctrl || !in_ctrl) return false;

    es8311_codec_cfg_t es8311 = {
        .ctrl_if = out_ctrl, .gpio_if = gpio_if, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,                                 // the amplifier is on the expander
        .use_mclk = true, .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    es7210_codec_cfg_t es7210 = {
        .ctrl_if = in_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *out_codec = es8311_codec_new(&es8311);
    const audio_codec_if_t *in_codec = es7210_codec_new(&es7210);
    if (!out_codec || !in_codec) return false;

    esp_codec_dev_cfg_t out_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = out_codec, .data_if = data_if};
    esp_codec_dev_cfg_t in_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = in_codec, .data_if = data_if};
    audio.out = esp_codec_dev_new(&out_cfg);
    audio.in = esp_codec_dev_new(&in_cfg);
    if (!audio.out || !audio.in) return false;

    esp_codec_dev_sample_info_t play = {.bits_per_sample = 16, .channel = 1, .sample_rate = rate};
    esp_codec_dev_sample_info_t record = {.bits_per_sample = 16, .channel = BOARD_SLOTS,
                                          .channel_mask = BOARD_TAKE, .sample_rate = rate};
    if (esp_codec_dev_open(audio.out, &play) != 0 || esp_codec_dev_open(audio.in, &record) != 0) return false;
    esp_codec_dev_set_out_vol(audio.out, 70);
    esp_codec_dev_set_in_channel_gain(audio.in, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1), settings.mic_gain_x2 / 2.0f);
    return true;
}

// The reset button restarts this chip only: the six chips on the bus keep
// their power, and one cut off mid-answer holds the data line down until it is
// clocked out. So before the driver takes the pins: if SDA is held, up to nine
// clock pulses and a STOP, by hand. Not i2c_master_bus_reset: called at boot it
// left the ES8311 unreachable for the whole of codecs_init, three boots of
// three (2026-10-09), and no sound came up.
static void bus_clear(int sda, int scl)
{
    gpio_config_t pins = {.pin_bit_mask = 1ULL << sda | 1ULL << scl, .mode = GPIO_MODE_INPUT_OUTPUT_OD, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&pins);
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(10);
    int pulses = 0;
    while (!gpio_get_level(sda) && pulses < 9) {
        gpio_set_level(scl, 0); esp_rom_delay_us(5);
        gpio_set_level(scl, 1); esp_rom_delay_us(5);
        pulses++;
    }
    if (pulses) {
        bus_held = pulses;
        ESP_LOGW(TAG, "the I2C data line was held down at boot: %d clock pulses %s it", pulses, gpio_get_level(sda) ? "freed" : "did NOT free");
        gpio_set_level(scl, 0); esp_rom_delay_us(5);
        gpio_set_level(sda, 0); esp_rom_delay_us(5);
        gpio_set_level(scl, 1); esp_rom_delay_us(5);
        gpio_set_level(sda, 1); esp_rom_delay_us(5);          // STOP
    }
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
}

bool board_init(int rate)
{
    i2c_master_bus_config_t bus = {.i2c_port = 0, .sda_io_num = GPIO_NUM_11, .scl_io_num = GPIO_NUM_10,
                                   .clk_source = I2C_CLK_SRC_DEFAULT};
    bus_clear(bus.sda_io_num, bus.scl_io_num);
    if (i2c_new_master_bus(&bus, &i2c) != ESP_OK) { ESP_LOGE(TAG, "%s", trouble = "the I2C bus did not start"); return false; }
    bool have = false;
    for (int n = 0; n < 3 && !(have = expander_init()); n++) {
        ESP_LOGW(TAG, "the TCA9555 did not answer (try %d)", n + 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!have) { ESP_LOGE(TAG, "%s", trouble = "the port expander (TCA9555) did not answer: no glass, no sound, no card"); return false; }
    if (!i2s_init(rate)) { ESP_LOGE(TAG, "%s", trouble = "I2S did not start: no sound"); return false; }
    if (!codecs_init(rate)) { ESP_LOGE(TAG, "%s", trouble = "a sound chip (ES8311 or ES7210) did not answer: no sound"); return false; }
    ESP_LOGI(TAG, "%s ready at %d Hz", BOARD_NAME, rate);
    return true;
}

board_audio_t *board_audio(void) { return &audio; }
const char *board_trouble(void) { return trouble; }
void board_i2c_counts(int *errors, int *resets, int *held) { *errors = touch_errors; *resets = bus_resets; *held = bus_held; }

bool board_sd_select(bool high)
{
    if (!expander) return false;
    return esp_io_expander_set_level(expander, PIN_SD_CS, high ? 1 : 0) == ESP_OK;
}

void board_camera_power(bool on)
{
    if (expander) esp_io_expander_set_level(expander, PIN_CAMERA_DOWN, on ? 0 : 1);   // PWDN is active high
}

void board_amplifier(bool on)
{
    if (expander) esp_io_expander_set_level(expander, PIN_PA, on);
}

// Who answers on the shared bus: 0x18 ES8311, 0x20 TCA9555, 0x40 ES7210, and
// with the plugs filled 0x15 (CST816 touch) and 0x3c (an OV camera's SCCB,
// only while EXIO5 powers it).
int board_i2c_scan(cJSON *list)
{
    if (!i2c) return -1;
    int found = 0;
    for (int addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(i2c, addr, 50) == ESP_OK) { cJSON_AddItemToArray(list, cJSON_CreateNumber(addr)); found++; }
    }
    return found;
}

// --- the glass ---------------------------------------------------------------------

board_glass_t *board_glass(void) { return &glass; }

// One poll of the touch chip, from LVGL's task. A failed read is a lifted
// finger and a count, never a stop: esp_lvgl_port's own reader aborts the
// program on any error (ESP_ERROR_CHECK), so the glass does not use it.
bool board_touch_read(int *x, int *y)
{
    if (!glass.touch) return false;
    int64_t now = esp_timer_get_time();
    if (now < touch_rest_until) return false;
    if (esp_lcd_touch_read_data(glass.touch) != ESP_OK) {
        touch_errors++;
        if (++touch_bad >= TOUCH_BAD_ROW) {
            touch_bad = 0;
            bus_resets++;                             // the driver resets its own state after a timeout; a rest lets the bus settle
            ESP_LOGW(TAG, "the touch chip stopped answering for a second (%d times so far)", bus_resets);
            touch_rest_until = now + 1000000;
        }
        return false;
    }
    touch_bad = 0;
    esp_lcd_touch_point_data_t p[1];
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(glass.touch, p, &n, 1) != ESP_OK || !n) return false;
    *x = p[0].x;
    *y = p[0].y;
    return true;
}

void board_backlight(int percent)
{
    if (!glass.up) return;
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 255 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void board_glass_orient(bool invert, bool flip)
{
    if (!glass.panel) return;
    esp_lcd_panel_invert_color(glass.panel, invert);
    esp_lcd_panel_swap_xy(glass.panel, true);          // landscape: a quarter turn; flip = the other quarter (180 from it)
    esp_lcd_panel_mirror(glass.panel, !flip, flip);
    if (glass.touch) {
        esp_lcd_touch_set_swap_xy(glass.touch, true);
        esp_lcd_touch_set_mirror_x(glass.touch, !flip);
        esp_lcd_touch_set_mirror_y(glass.touch, flip);
    }
}

bool board_glass_init(void)
{
    if (!i2c || !expander) { ESP_LOGW(TAG, "no glass: the bus is not up"); return false; }
    // the backlight: PWM at 25 kHz, 8 bit, dark until the screen is built. Not 5 kHz:
    // that sat in the audio band and the quiet room read 4x louder on the mics with
    // the backlight dimmed (2026-10-07); above 8 kHz the 16 kHz mic path cannot see it.
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                             .timer_num = LEDC_TIMER_0, .freq_hz = 25000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&t);
    ledc_channel_config_t c = {.gpio_num = BOARD_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                               .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0};
    ledc_channel_config(&c);
    // the panel. Its reset line is EXIO0, pulsed in expander_init already.
    spi_bus_config_t bus = {.sclk_io_num = BOARD_LCD_SCK, .mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = -1,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = BOARD_W * BOARD_DRAW_LINES * 2};
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG, "the glass's SPI bus did not start"); return false; }
    esp_lcd_panel_io_spi_config_t io = {.dc_gpio_num = BOARD_LCD_DC, .cs_gpio_num = BOARD_LCD_CS, .pclk_hz = BOARD_LCD_HZ,
                                        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
                                        .flags.psram_dma_direct = true};      // LVGL's draw buffers live in PSRAM: the DMA reads them there
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &glass.io) != ESP_OK) return false;
    esp_lcd_panel_dev_config_t panel = {.reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16};
    if (esp_lcd_new_panel_st7789(glass.io, &panel, &glass.panel) != ESP_OK) { ESP_LOGE(TAG, "no ST7789"); return false; }
    esp_lcd_panel_reset(glass.panel);
    esp_lcd_panel_init(glass.panel);
    // the touch chip: a CST816 at 0x15 on the shared bus, polled (its INT is on the expander)
    esp_lcd_panel_io_i2c_config_t tp_io = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io.scl_speed_hz = 400000;
    tp_io.transaction_timeout_ms = TOUCH_TIMEOUT_MS;
    esp_lcd_panel_io_handle_t tp_io_h = NULL;
    esp_lcd_touch_config_t tp = {.x_max = BOARD_PANEL_W, .y_max = BOARD_PANEL_H, .rst_gpio_num = -1, .int_gpio_num = -1,
                                 .levels = {.reset = 0, .interrupt = 0}};
    if (esp_lcd_new_panel_io_i2c(i2c, &tp_io, &tp_io_h) != ESP_OK
        || esp_lcd_touch_new_i2c_cst816s(tp_io_h, &tp, &glass.touch) != ESP_OK) {
        ESP_LOGW(TAG, "no touch chip answered at 0x15: the glass shows, taps do nothing");
        glass.touch = NULL;
    }
    // a sleeping CST816 answers nothing between touches; the poll would log an error 30 times a second
    esp_log_level_set("CST816S", ESP_LOG_NONE);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    glass.up = true;
    board_glass_orient(settings.invert, settings.flip);
    esp_lcd_panel_disp_on_off(glass.panel, true);
    ESP_LOGI(TAG, "glass up: ST7789 %dx%d landscape at %d MHz, touch %s", BOARD_W, BOARD_H, BOARD_LCD_HZ / 1000000, glass.touch ? "CST816" : "MISSING");
    return true;
}
