// board.c - see board.h. The AW9523 is driven by hand (sixty lines, no component
// in the registry does it): outputs on port 1, LED current sinks for the
// backlight and the status LED.
#include "board.h"
#include "audio.h"
#include "settings.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board";
static i2c_master_bus_handle_t i2c;
static i2c_master_dev_handle_t aw;     // the expander
static i2c_master_dev_handle_t tp_raw; // the touch chip, for the one register its driver does not write
static SemaphoreHandle_t aw_lock;
static board_audio_t audio;
static board_glass_t glass;
static const char *trouble = "";
static int touch_errors, touch_bad, bus_resets, bus_held;
static int64_t touch_rest_until;
static int speaker_heals;
static char speaker_last[64] = "";
#define TOUCH_TIMEOUT_MS 30
#define TOUCH_BAD_ROW 30

// --- the AW9523 -----------------------------------------------------------------------
#define AW_ADDR 0x5A                   // AD1 high, AD0 low
#define AW_REG_OUT0 0x02
#define AW_REG_OUT1 0x03
#define AW_REG_CFG0 0x04               // 1 = input, 0 = output
#define AW_REG_CFG1 0x05
#define AW_REG_ID 0x10                 // answers 0x23
#define AW_REG_GCR 0x11                // bit 4: port 0 push-pull; bits 1..0: LED current, 00 = the full 37 mA
#define AW_REG_LEDMODE0 0x12           // 1 = GPIO, 0 = LED current sink
#define AW_REG_LEDMODE1 0x13
#define AW_REG_RESET 0x7F
// DIM registers: 0x20..0x23 = P1_0..P1_3, 0x24..0x2B = P0_0..P0_7, 0x2C..0x2F = P1_4..P1_7
#define AW_DIM_BL 0x23                 // P1_3: the backlight's cathode
#define AW_DIM_G 0x28                  // P0_4
#define AW_DIM_R 0x29                  // P0_5
#define AW_DIM_B 0x2A                  // P0_6
#define P1_PWR_EN 0x01                 // P1_0: high = the rail is on
#define P1_FILL 0x02                   // P1_1: high = the white LED is on
// Two revisions of the board. V1.0 (the Eagle schematic, and the board on the bench 2026-11-02: its touch
// chip answers with P1_4 low, its amplifier goes quiet with P1_4 high): the touch reset is GPIO 12 and
// P1_4 is the amplifier's P-FET. V1.1 (Elecrow's BoardControl.h): P1_4 resets the touch chip and P1_5
// is the amplifier. BOARD_REV11 picks the second; P1_5 is driven the V1.1 way regardless (unconnected on V1.0).
#ifdef BOARD_REV11
#define P1_TP_RST 0x10                 // P1_4: high = the touch chip runs
#define P1_AMP_OFF 0x20                // P1_5: high = the amplifier is unpowered
#else
#define P1_TP_RST 0x00                 // no expander pin: the touch reset is GPIO 12
#define P1_AMP_OFF 0x10                // P1_4: high = the amplifier is unpowered
#endif
#define LEDMODE0 (uint8_t)~0x70        // P0_4, P0_5, P0_6 in LED mode
#define LEDMODE1 (uint8_t)~0x08        // P1_3 in LED mode
static uint8_t out1 = P1_PWR_EN | P1_TP_RST | P1_AMP_OFF | 0x20;   // what port 1 should show: rail on, touch out of reset, amplifier off (both revisions' pins), fill light off
static uint8_t dim_bl, dim_r, dim_g, dim_b;

static bool aw_regs(void);

static bool aw_write(uint8_t reg, uint8_t value)
{
    uint8_t two[2] = {reg, value};
    return aw && i2c_master_transmit(aw, two, 2, 50) == ESP_OK;
}

static bool aw_read(uint8_t reg, uint8_t *value)
{
    return aw && i2c_master_transmit_receive(aw, &reg, 1, value, 1, 50) == ESP_OK;
}

static bool aw_out1(void)
{
    xSemaphoreTake(aw_lock, portMAX_DELAY);
    bool ok = aw_write(AW_REG_OUT1, out1);
    xSemaphoreGive(aw_lock);
    return ok;
}

static bool expander_init(void)
{
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = AW_ADDR, .scl_speed_hz = BOARD_I2C_HZ};
    if (!aw && i2c_master_bus_add_device(i2c, &dev, &aw) != ESP_OK) { aw = NULL; return false; }
    if (!aw_lock) aw_lock = xSemaphoreCreateMutex();
    uint8_t id = 0;
    if (!aw_read(AW_REG_ID, &id) || id != 0x23) { ESP_LOGW(TAG, "AW9523 id read %02x", id); return false; }
    return aw_regs();
}

// Every register the expander is meant to hold, written afresh: at init, and whole again when it is
// found reset (board_speaker_check): OUT1 alone put the rail back but left the LED modes and currents
// at their defaults, a dark backlight and halo over a live glass (scouts 2026-11-05). Under aw_lock or before it exists.
static bool aw_regs(void)
{
    bool ok = aw_write(AW_REG_GCR, 0x10);
    ok = ok && aw_write(AW_REG_CFG0, 0x00) && aw_write(AW_REG_CFG1, 0x00);   // every pin an output
    ok = ok && aw_write(AW_REG_LEDMODE0, LEDMODE0) && aw_write(AW_REG_LEDMODE1, LEDMODE1);
    ok = ok && aw_write(AW_DIM_BL, 0) && aw_write(AW_DIM_R, 0) && aw_write(AW_DIM_G, 0) && aw_write(AW_DIM_B, 0);
    dim_bl = dim_r = dim_g = dim_b = 0;                                      // the caches say what the chip now holds: the next backlight and halo writes go through
    ok = ok && aw_write(AW_REG_OUT0, 0x00) && aw_write(AW_REG_OUT1, out1);   // the rail comes on here
    return ok;
}

bool board_rail(bool on)
{
    if (!aw) return false;
    out1 = on ? (out1 | P1_PWR_EN) : (uint8_t)(out1 & ~P1_PWR_EN);
    // Written until the chip reads it back: at a boot the expander's first transaction has failed
    // (the bus not settled: "did not answer (try 1)", 2026-11-05), and a wake that lost this write
    // ran on with the rail off, no glass, no mic, the log saying "awake" (scouts 2026-11-05).
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t back = 0;
        xSemaphoreTake(aw_lock, portMAX_DELAY);
        ok = aw_write(AW_REG_OUT1, out1) && aw_read(AW_REG_OUT1, &back) && back == out1;
        xSemaphoreGive(aw_lock);
    }
    if (!ok) ESP_LOGE(TAG, "the rail write did not take (%s)", on ? "on" : "off");
    if (on) vTaskDelay(pdMS_TO_TICKS(50));          // the regulators behind it, and the panel's RC reset
    return ok;
}
bool board_rail_on(void) { return out1 & P1_PWR_EN; }

void board_fill_light(bool on)
{
    if (!aw) return;
    out1 = on ? (out1 | P1_FILL) : (uint8_t)(out1 & ~P1_FILL);
    aw_out1();
}
bool board_fill_light_on(void) { return out1 & P1_FILL; }

void board_amplifier(bool on)
{
    if (!aw) return;
    out1 = on ? (uint8_t)(out1 & ~P1_AMP_OFF) : (out1 | P1_AMP_OFF);
    aw_out1();
}

void board_led(uint8_t r, uint8_t g, uint8_t b)
{
    if (!aw) return;
    xSemaphoreTake(aw_lock, portMAX_DELAY);
    if (r != dim_r) aw_write(AW_DIM_R, dim_r = r);
    if (g != dim_g) aw_write(AW_DIM_G, dim_g = g);
    if (b != dim_b) aw_write(AW_DIM_B, dim_b = b);
    xSemaphoreGive(aw_lock);
}

void board_backlight(int percent)
{
    if (!aw) return;
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    uint8_t v = percent ? 2 + percent * percent * 253 / 10000 : 0;   // a square: 5% = 2 of 255 (Krem wanted it dimmer than a line gave), 50% = 65, 100% = 255
    xSemaphoreTake(aw_lock, portMAX_DELAY);
    if (v != dim_bl) aw_write(AW_DIM_BL, dim_bl = v);
    xSemaphoreGive(aw_lock);
}

// The expander's port 1 read back and put right: the rail, the amplifier and the fill
// light are what it says. -1 unreadable, 0 fine, 1 healed.
int board_speaker_check(void)
{
    if (!aw) return -1;
    uint8_t have = 0;
    xSemaphoreTake(aw_lock, portMAX_DELAY);
    bool read = aw_read(AW_REG_OUT1, &have);
    bool fix = read && have != out1;
    if (fix) aw_regs();                                   // whole: a chip that lost port 1 lost its LED modes and currents too
    xSemaphoreGive(aw_lock);
    if (!read) return -1;
    if (!fix) return 0;
    speaker_heals++;
    snprintf(speaker_last, sizeof speaker_last, "expander port 1 read %02x, wanted %02x", have, out1);
    ESP_LOGW(TAG, "the expander had lost its outputs (%s): put back (%d times so far)", speaker_last, speaker_heals);
    return 1;
}
int board_speaker_heals(const char **last) { *last = speaker_last; return speaker_heals; }

// --- I2S ------------------------------------------------------------------------------
// The mic's DMA ring overflowed: the driver threw its oldest descriptor away. Counted in
// samples so audio.c can keep the reference ring's index true across the hole (0.1.23).
static volatile uint32_t mic_dropped;
static bool IRAM_ATTR on_mic_overflow(i2s_chan_handle_t h, i2s_event_data_t *ev, void *user)
{
    mic_dropped += ev->size / sizeof(int16_t);
    return false;
}
uint32_t board_mic_dropped(void) { return mic_dropped; }

// The amplifier's DMA cycles whether or not anything is written (auto_clear plays silence):
// every descriptor that ends is counted, so this is the speaker's clock in frames (audio.c
// measures its real rate against the timer, as it does the mic's, and resamples the echo
// reference by the ratio: the two dividers are not the same ppm off, 0.1.26).
static volatile uint64_t spk_sent;
static volatile int64_t spk_sent_us;
static bool IRAM_ATTR on_spk_sent(i2s_chan_handle_t h, i2s_event_data_t *ev, void *user)
{
    spk_sent += ev->size / (2 * sizeof(int16_t));   // stereo 16 bit: four bytes a frame
    spk_sent_us = esp_timer_get_time();
    return false;
}
void board_spk_sent(uint64_t *frames, int64_t *at_us) { *frames = spk_sent; *at_us = spk_sent_us; }

static bool i2s_init(int rate)
{
    i2s_chan_config_t rxc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);   // only I2S0 decodes PDM
    rxc.dma_desc_num = 8;                          // 160 ms of mic in flight: the feed task is below WiFi and lwIP on its core,
    rxc.dma_frame_num = 320;                       // and 60 ms (four) lost a descriptor on most sentences (0.1.23)
    if (i2s_new_channel(&rxc, NULL, &audio.rx) != ESP_OK) return false;
    i2s_event_callbacks_t cbs = {.on_recv_q_ovf = on_mic_overflow};
    i2s_channel_register_event_callback(audio.rx, &cbs, NULL);
    i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = BOARD_MIC_CLK, .din = BOARD_MIC_DATA, .invert_flags = {.clk_inv = false}},
    };
    pdm.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;    // L/R is tied to ground
    if (i2s_channel_init_pdm_rx_mode(audio.rx, &pdm) != ESP_OK) return false;
    // the amplifier: six descriptors of 160 frames = 40 ms of sound in flight at 24 kHz (60 at 16);
    // a first write lands in the next one to play (the queue is one short of the ring), audio.c
    i2s_chan_config_t txc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    txc.dma_desc_num = 12;                         // 80 ms of her voice in flight (was 6 = 40: at 25 frames a second the feeder lost the race to WiFi bursts and popped, 0.2.3)
    txc.dma_frame_num = BOARD_SPK_DESC_FRAMES;
    txc.auto_clear_after_cb = true;
    if (i2s_new_channel(&txc, &audio.tx, NULL) != ESP_OK) return false;
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(settings.speaker_rate),   // her voice, 16 or 24 kHz; the mic's clock is the other port's
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = BOARD_SPK_BCLK, .ws = BOARD_SPK_WS, .dout = BOARD_SPK_DOUT, .din = I2S_GPIO_UNUSED},
    };
    if (i2s_channel_init_std_mode(audio.tx, &std) != ESP_OK) return false;
    i2s_event_callbacks_t tcb = {.on_sent = on_spk_sent};
    i2s_channel_register_event_callback(audio.tx, &tcb, NULL);
    return i2s_channel_enable(audio.rx) == ESP_OK && i2s_channel_enable(audio.tx) == ESP_OK;
}

// If SDA is held low by a chip cut off mid-answer (the reset button restarts this chip
// only), clock it free before the driver takes the pins.
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
        gpio_set_level(sda, 1); esp_rom_delay_us(5);
    }
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
}

// Both the V1.0 line (GPIO 12) and the V1.1 one (the expander's P1_4) are pulsed: one of them is the chip's.
static void touch_reset_pulse(void)
{
    gpio_set_level(BOARD_TP_RST, 0);
    out1 &= ~P1_TP_RST;
    if (P1_TP_RST) aw_out1();
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TP_RST, 1);
    out1 |= P1_TP_RST;
    if (P1_TP_RST) aw_out1();
    vTaskDelay(pdMS_TO_TICKS(60));                  // the CST816 wants ~50 ms after reset before it answers
}

static bool i2c_up(void)
{
    if (i2c) return true;
    i2c_master_bus_config_t bus = {.i2c_port = 0, .sda_io_num = BOARD_I2C_SDA, .scl_io_num = BOARD_I2C_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .flags.enable_internal_pullup = true};
    bus_clear(bus.sda_io_num, bus.scl_io_num);
    return i2c_new_master_bus(&bus, &i2c) == ESP_OK;
}

// Before board_init, from power.c at a dead-battery boot. The expander is not reset with the chip,
// so the rail and the amplifier are as the last life left them: on. This boot is going to deep-sleep
// instead of booting, and they go off first: the bus, the device, every pin an output, every output
// off. One tick from the speaker as the amplifier loses power; the loop this replaces ticked twice
// a lap. No lock: nothing else is up yet.
bool board_dark(void)
{
    if (!i2c_up()) return false;
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = AW_ADDR, .scl_speed_hz = BOARD_I2C_HZ};
    if (!aw && i2c_master_bus_add_device(i2c, &dev, &aw) != ESP_OK) { aw = NULL; return false; }
    out1 = (uint8_t)((out1 & ~P1_PWR_EN) | P1_AMP_OFF);
    return aw_write(AW_REG_CFG0, 0x00) && aw_write(AW_REG_CFG1, 0x00) && aw_write(AW_REG_OUT0, 0x00) && aw_write(AW_REG_OUT1, out1);
}

bool board_init(int rate)
{
    gpio_config_t pins = {.pin_bit_mask = 1ULL << BOARD_TP_RST, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&pins);
    gpio_set_level(BOARD_TP_RST, 0);
    gpio_config_t key = {.pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON | 1ULL << BOARD_TP_INT, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
    gpio_config(&key);
    if (!i2c_up()) { ESP_LOGE(TAG, "%s", trouble = "the I2C bus did not start"); return false; }
    bool have = false;
    for (int n = 0; n < 3 && !(have = expander_init()); n++) {
        ESP_LOGW(TAG, "the AW9523 did not answer (try %d)", n + 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!have) { ESP_LOGE(TAG, "%s", trouble = "the port expander (AW9523) did not answer: no rail, no glass, no sound"); return false; }
    vTaskDelay(pdMS_TO_TICKS(50));                  // the rail is up: the panel's RC reset, the touch chip's power
    touch_reset_pulse();
    if (!i2s_init(rate)) { ESP_LOGE(TAG, "%s", trouble = "I2S did not start: no sound"); return false; }
    ESP_LOGI(TAG, "%s ready: mic at %d Hz, speaker at %d Hz, rail on, amplifier off", BOARD_NAME, rate, settings.speaker_rate);
    return true;
}

board_audio_t *board_audio(void) { return &audio; }

bool board_speaker_rate(int rate)
{
    if (!audio.tx) return false;
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_disable(audio.tx);
    bool ok = i2s_channel_reconfig_std_clock(audio.tx, &clk) == ESP_OK;
    i2s_channel_enable(audio.tx);
    return ok;
}
const char *board_trouble(void) { return trouble; }
void board_i2c_counts(int *errors, int *resets, int *held) { *errors = touch_errors; *resets = bus_resets; *held = bus_held; }
bool board_boot_down(void) { return gpio_get_level(BOARD_BOOT_BUTTON) == 0; }
bool board_battery_read(board_battery_t *out) { memset(out, 0, sizeof *out); out->percent = -1; return false; }

// Who answers on the bus: 0x15 the touch chip (with the rail on), 0x5A the expander.
int board_i2c_scan(cJSON *list)
{
    if (!i2c) return -1;
    int found = 0;
    for (int addr = 0x08; addr < 0x78; addr++)
        if (i2c_master_probe(i2c, addr, 50) == ESP_OK) { cJSON_AddItemToArray(list, cJSON_CreateNumber(addr)); found++; }
    return found;
}

// --- the glass ---------------------------------------------------------------------

board_glass_t *board_glass(void) { return &glass; }

int board_touch_reg(int reg)
{
    if (!tp_raw) return -1;
    uint8_t r = reg, v = 0;
    return i2c_master_transmit_receive(tp_raw, &r, 1, &v, 1, 50) == ESP_OK ? v : -1;
}
bool board_touch_int(void) { return gpio_get_level(BOARD_TP_INT) == 0; }

bool board_touch_read(int *x, int *y)
{
    if (!glass.touch) return false;
    int64_t now = esp_timer_get_time();
    if (now < touch_rest_until) return false;
    if (esp_lcd_touch_read_data(glass.touch) != ESP_OK) {
        touch_errors++;
        if (++touch_bad >= TOUCH_BAD_ROW) {
            touch_bad = 0;
            bus_resets++;
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

static int gap_now;
void board_glass_gap(int y) { if (glass.panel) { gap_now = y; esp_lcd_panel_set_gap(glass.panel, 0, y); } }
int board_glass_gap_now(void) { return gap_now; }
void board_glass_orient(bool invert, bool flip, bool mirror)
{
    if (!glass.panel) return;
    // The glass shows 284 of the controller's 320 rows. Unturned, rows 0-283; turned 180 (the Y
    // axis mirrored) the glass sits over rows 36-319, so the gap moves to the top: without it the
    // picture began 36 rows down, its top cut off and the bottom showing whatever the memory held
    // (Krem, 2026-11-03). Those rows are cleared first, with no mirror and no gap, so nothing
    // stale can show in either orientation.
    static uint16_t *black;
    if (!black) black = heap_caps_calloc((size_t)BOARD_PANEL_W * 40, 2, MALLOC_CAP_SPIRAM);
    if (black) {
        esp_lcd_panel_mirror(glass.panel, false, false);
        esp_lcd_panel_set_gap(glass.panel, 0, 0);
        for (int y = 0; y < BOARD_PANEL_ROWS; y += 40) esp_lcd_panel_draw_bitmap(glass.panel, 0, y, BOARD_PANEL_W, y + 40 > BOARD_PANEL_ROWS ? BOARD_PANEL_ROWS : y + 40, black);
    }
    esp_lcd_panel_invert_color(glass.panel, invert);
    esp_lcd_panel_swap_xy(glass.panel, false);         // portrait is the panel's own way
    gap_now = flip ? BOARD_GAP_TURNED : BOARD_GAP_UP;
    esp_lcd_panel_set_gap(glass.panel, 0, gap_now);
    esp_lcd_panel_mirror(glass.panel, flip ^ mirror, flip);   // 180 degrees = both axes; mirror = the x axis alone
    if (glass.touch) {
        esp_lcd_touch_set_swap_xy(glass.touch, false);
        esp_lcd_touch_set_mirror_x(glass.touch, BOARD_TOUCH_MIRROR_X ^ flip ^ mirror);
        esp_lcd_touch_set_mirror_y(glass.touch, BOARD_TOUCH_MIRROR_Y ^ flip);
    }
}

// The CST816 sleeps a couple of seconds after the last finger and answers nothing until
// the next one: a poll then misses taps for a second at a time. Register 0xFE = 1 keeps it awake.
static void touch_stay_awake(void)
{
    if (!tp_raw) {
        i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x15, .scl_speed_hz = BOARD_I2C_HZ};
        if (i2c_master_bus_add_device(i2c, &dev, &tp_raw) != ESP_OK) { tp_raw = NULL; return; }
    }
    uint8_t two[2] = {0xFE, 0x01};
    if (i2c_master_transmit(tp_raw, two, 2, 50) != ESP_OK) ESP_LOGW(TAG, "the touch chip did not take 'stay awake'");
}

static bool panel_up(void)
{
    if (esp_lcd_panel_reset(glass.panel) != ESP_OK || esp_lcd_panel_init(glass.panel) != ESP_OK) return false;
    board_glass_orient(settings.invert, settings.flip, settings.mirror);   // the gap is its to set
    esp_lcd_panel_disp_on_off(glass.panel, true);
    return true;
}

bool board_glass_init(void)
{
    if (!i2c || !aw) { ESP_LOGW(TAG, "no glass: the bus is not up"); return false; }
    spi_bus_config_t bus = {.sclk_io_num = BOARD_LCD_SCK, .mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = -1,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = BOARD_W * BOARD_DRAW_LINES * 2};
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG, "the glass's SPI bus did not start"); return false; }
    esp_lcd_panel_io_spi_config_t io = {.dc_gpio_num = BOARD_LCD_DC, .cs_gpio_num = BOARD_LCD_CS, .pclk_hz = BOARD_LCD_HZ,
                                        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
                                        .flags.psram_dma_direct = true};
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &glass.io) != ESP_OK) return false;
    esp_lcd_panel_dev_config_t panel = {.reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16};
    if (esp_lcd_new_panel_st7789(glass.io, &panel, &glass.panel) != ESP_OK) { ESP_LOGE(TAG, "no ST7789"); return false; }
    if (!panel_up()) { ESP_LOGE(TAG, "the panel did not take its setup"); return false; }
    esp_lcd_panel_io_i2c_config_t tp_io = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io.scl_speed_hz = BOARD_I2C_HZ;
    tp_io.transaction_timeout_ms = TOUCH_TIMEOUT_MS;
    esp_lcd_panel_io_handle_t tp_io_h = NULL;
    esp_lcd_touch_config_t tp = {.x_max = BOARD_PANEL_W, .y_max = BOARD_PANEL_H, .rst_gpio_num = -1, .int_gpio_num = -1,
                                 .levels = {.reset = 0, .interrupt = 0}};
    if (esp_lcd_new_panel_io_i2c(i2c, &tp_io, &tp_io_h) != ESP_OK
        || esp_lcd_touch_new_i2c_cst816s(tp_io_h, &tp, &glass.touch) != ESP_OK) {
        ESP_LOGW(TAG, "no touch chip answered at 0x15: the glass shows, taps do nothing");
        glass.touch = NULL;
    } else touch_stay_awake();
    esp_log_level_set("CST816S", ESP_LOG_NONE);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    glass.up = true;
    board_glass_orient(settings.invert, settings.flip, settings.mirror);
    ESP_LOGI(TAG, "glass up: %dx%d portrait at %d MHz, touch %s", BOARD_W, BOARD_H, BOARD_LCD_HZ / 1000000, glass.touch ? "CST816" : "MISSING");
    return true;
}

// The rail was off: the panel forgot everything and the touch chip was unpowered.
bool board_glass_wake(void)
{
    if (!glass.up) return false;
    touch_reset_pulse();
    if (!panel_up()) return false;
    if (glass.touch) touch_stay_awake();
    touch_rest_until = 0;
    touch_bad = 0;
    return true;
}
