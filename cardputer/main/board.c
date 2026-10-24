// board.c - Cardputer ADV bring-up (see board.h). P2 step 1: I2C bus, I2S
// (full-duplex std), and the ES8311 opened for microphone capture, so the
// console `mic` command can tell us whether the v5.5.x ES8311 mic regression
// (#18621) bites this board before we build the whole PTT path on top of it.
#include "board.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";
static const char *trouble = "";
static i2c_master_bus_handle_t i2c;
static i2s_chan_handle_t tx, rx;
static esp_codec_dev_handle_t mic;
static esp_codec_dev_handle_t spk;             // ES8311 DAC out (TTS playback)
static i2c_master_dev_handle_t tca;            // TCA8418 keyboard controller
static bool kbd_ready;
static volatile bool kbd_held;                 // while a console key-dump runs, PTT stops draining the FIFO

// TCA8418 keypad-scanner registers (TI datasheet)
#define TCA_CFG        0x01
#define TCA_INT_STAT   0x02
#define TCA_KEY_LCK_EC 0x03                     // low nibble = events waiting in the FIFO
#define TCA_KEY_EVENT  0x04                     // read to pop the oldest (bit7 down/up, bits6:0 key#)
#define TCA_KP_GPIO1   0x1D                     // ROW0-7 -> keypad
#define TCA_KP_GPIO2   0x1E                     // COL0-7 -> keypad
#define TCA_KP_GPIO3   0x1F                     // COL8-9 -> keypad

static bool i2c_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = 0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus, &i2c) != ESP_OK) { trouble = "the I2C bus did not start"; return false; }
    return true;
}

// Full-duplex I2S on one port: speaker on DOUT, mic on DIN, shared BCLK/WS,
// the board is master so it clocks the ES8311; MCLK is derived from BCLK.
static bool i2s_init(int rate)
{
    i2s_chan_config_t chan = {
        .id = I2S_NUM_0, .role = I2S_ROLE_MASTER, .dma_desc_num = 6, .dma_frame_num = 240,
        .auto_clear_after_cb = true,
    };
    if (i2s_new_channel(&chan, &tx, &rx) != ESP_OK) { trouble = "i2s channel"; return false; }
    i2s_std_config_t std = {
        .clk_cfg = {.sample_rate_hz = rate, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = BOARD_I2S_BCLK, .ws = BOARD_I2S_WS,
                     .dout = BOARD_I2S_DOUT, .din = BOARD_I2S_DIN},
    };
    if (i2s_channel_init_std_mode(tx, &std) != ESP_OK) { trouble = "i2s tx std"; return false; }
    if (i2s_channel_init_std_mode(rx, &std) != ESP_OK) { trouble = "i2s rx std"; return false; }
    if (i2s_channel_enable(tx) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) { trouble = "i2s enable"; return false; }
    return true;
}

static bool es8311_init(int rate)
{
    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_cfg = {.port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!data_if || !gpio_if || !ctrl_if) { trouble = "codec interfaces"; return false; }

    es8311_codec_cfg_t cfg = {
        .ctrl_if = ctrl_if, .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,    // mic (PTT) and speaker (TTS) on one full-duplex ES8311
        .pa_pin = -1,                                   // NS4150B: no PA-enable GPIO (output via codec reg)
        .use_mclk = false,                              // no MCLK pin; derived from BCLK
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec = es8311_codec_new(&cfg);
    if (!codec) { trouble = "es8311 new"; return false; }

    // IN (mic) and OUT (speaker) share the one codec_if + data_if (same I2S port, full duplex).
    esp_codec_dev_cfg_t in_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if};
    esp_codec_dev_cfg_t out_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if};
    mic = esp_codec_dev_new(&in_cfg);
    spk = esp_codec_dev_new(&out_cfg);
    if (!mic || !spk) { trouble = "codec dev"; return false; }
    esp_codec_dev_sample_info_t rec = {.bits_per_sample = 16, .channel = 1, .sample_rate = rate};
    esp_codec_dev_sample_info_t play = {.bits_per_sample = 16, .channel = 1, .sample_rate = rate};
    if (esp_codec_dev_open(mic, &rec) != 0) { trouble = "codec open (in)"; return false; }
    if (esp_codec_dev_open(spk, &play) != 0) { trouble = "codec open (out)"; return false; }
    esp_codec_dev_set_in_channel_gain(mic, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), 30.0f);
    esp_codec_dev_set_out_vol(spk, 100);           // full codec DAC level; the NS4150B amp gain is fixed in HW
    return true;
}

int board_speaker_write(const int16_t *pcm, int samples)
{
    if (!spk) return -1;
    if (esp_codec_dev_write(spk, (void *)pcm, (size_t)samples * sizeof(int16_t)) != 0) return -1;
    return samples;
}

void board_speaker_vol(int percent) { if (spk) esp_codec_dev_set_out_vol(spk, percent); }

static esp_err_t tca_w(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(tca, buf, 2, 100);
}

static uint8_t tca_r(uint8_t reg)
{
    uint8_t v = 0;
    i2c_master_transmit_receive(tca, &reg, 1, &v, 1, 100);
    return v;
}

// Put the whole 8x10 grid into keypad mode (covers the real 7x8) and drain any
// stale events. Best-effort: audio does not depend on the keyboard being there.
bool board_kbd_init(void)
{
    if (kbd_ready) return true;
    if (!i2c) return false;
    if (i2c_master_probe(i2c, BOARD_TCA8418, 100) != ESP_OK) return false;   // absent / not wired
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TCA8418,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(i2c, &dev, &tca) != ESP_OK) return false;
    tca_w(TCA_KP_GPIO1, 0xFF);                   // every row...
    tca_w(TCA_KP_GPIO2, 0xFF);                   // ...and column...
    tca_w(TCA_KP_GPIO3, 0x03);                   // ...scanned as keypad
    tca_w(TCA_CFG, 0x01);                        // flag key events in INT_STAT
    tca_w(TCA_INT_STAT, 0x0F);                   // clear stale flags
    while (tca_r(TCA_KEY_LCK_EC) & 0x0F) tca_r(TCA_KEY_EVENT);   // empty the FIFO
    kbd_ready = true;
    ESP_LOGI(TAG, "TCA8418 keypad up");
    return true;
}

void board_kbd_hold(bool on) { kbd_held = on; }   // one consumer at a time: a console dump vs the PTT task
bool board_kbd_busy(void) { return kbd_held; }

bool board_kbd_read(int *key, bool *down)
{
    if (!kbd_ready) return false;
    if ((tca_r(TCA_KEY_LCK_EC) & 0x0F) == 0) return false;
    uint8_t ev = tca_r(TCA_KEY_EVENT);
    if (!ev) return false;
    if (down) *down = ev & 0x80;
    if (key)  *key = ev & 0x7F;
    return true;
}

bool board_init(int rate)
{
    if (!i2c_init()) return false;
    if (!i2s_init(rate)) return false;
    if (!es8311_init(rate)) return false;
    if (!board_kbd_init()) ESP_LOGW(TAG, "TCA8418 keyboard not found at 0x%02x", BOARD_TCA8418);
    ESP_LOGI(TAG, "board up: ES8311 mic open at %d Hz", rate);
    return true;
}

int board_mic_read(int16_t *buf, int samples)
{
    if (!mic) return -1;
    if (esp_codec_dev_read(mic, buf, (size_t)samples * sizeof(int16_t)) != 0) return -1;
    return samples;
}

const char *board_trouble(void) { return trouble; }
