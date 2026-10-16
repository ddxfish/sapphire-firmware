// speaker.c - see speaker.h.
#include "speaker.h"

#include <math.h>

#include <string.h>

#include "driver/dac_continuous.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "settings.h"

static const char *TAG = "speaker";

#define DMA_BUF 1024                   // bytes per DMA buffer (8-bit samples): 46 ms at 22.05 kHz
#define DMA_BUFS 6                     // 6 KB in all; a sentence streams in 4 KB at a time
#define CHUNK 512                      // 16-bit frames turned into 8-bit per write

static dac_continuous_handle_t dac;
static bool enabled;
static uint8_t out[CHUNK];
static volatile bool toning;           // a tone task is up

bool speaker_start(void)
{
    if (dac) return true;
    dac_continuous_config_t cfg = {
        .chan_mask = BOARD_AMP == 26 ? DAC_CHANNEL_MASK_CH1 : DAC_CHANNEL_MASK_CH0,
        .desc_num = DMA_BUFS,
        .buf_size = DMA_BUF,
        .freq_hz = SPEAKER_RATE,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    esp_err_t err = dac_continuous_new_channels(&cfg, &dac);
    if (err != ESP_OK) { ESP_LOGW(TAG, "no DAC channel (%s): her voice is off", esp_err_to_name(err)); dac = NULL; return false; }
    ESP_LOGI(TAG, "DAC on GPIO %d, %d Hz, volume %d", BOARD_AMP, SPEAKER_RATE, speaker_volume());
    return true;
}

bool speaker_ok(void) { return dac != NULL; }

static void wake(void)
{
    if (!dac || enabled) return;
    if (dac_continuous_enable(dac) == ESP_OK) enabled = true;
}

static void sleep_dac(void)
{
    if (!dac || !enabled) return;
    dac_continuous_disable(dac);
    enabled = false;
}

// 16-bit signed, any channel count, to 8-bit unsigned around 128, at the volume.
void speaker_play(const int16_t *frames, size_t samples, int channels)
{
    if (!dac || !samples) return;
    wake();
    int gain = speaker_volume();                   // 0..100, applied in the 16-bit domain
    if (channels < 1) channels = 1;
    size_t frame_count = samples / channels;
    for (size_t at = 0; at < frame_count;) {
        size_t n = frame_count - at < CHUNK ? frame_count - at : CHUNK;
        for (size_t i = 0; i < n; i++) {
            int32_t v = 0;
            for (int c = 0; c < channels; c++) v += frames[(at + i) * channels + c];
            v = v / channels * gain / 100;
            out[i] = (uint8_t)((v >> 8) + 128);
        }
        size_t loaded = 0, done = 0;
        while (done < n) {                           // the DMA takes what fits; wait for the rest
            if (dac_continuous_write(dac, out + done, n - done, &loaded, 1000) != ESP_OK) break;
            done += loaded;
        }
        at += n;
    }
}

static void tone_task(void *arg)
{
    int hz = (int)(intptr_t)arg >> 16, ms = (int)(intptr_t)arg & 0xFFFF;
    int16_t piece[256];
    size_t total = (size_t)SPEAKER_RATE * ms / 1000, done = 0;
    while (done < total) {
        size_t n = total - done < 256 ? total - done : 256;
        for (size_t i = 0; i < n; i++) piece[i] = (int16_t)(12000 * sinf(6.2831853f * hz * (done + i) / SPEAKER_RATE));
        speaker_play(piece, n, 1);
        done += n;
    }
    speaker_play_end();
    toning = false;
    vTaskDelete(NULL);
}

void speaker_tone(int hz, int ms)
{
    if (!dac || enabled || toning || hz < 50 || hz > 5000 || ms < 20 || ms > 3000) return;   // one at a time, never over her voice: the DAC and `out` are shared
    toning = true;
    if (xTaskCreate(tone_task, "tone", 6144, (void *)(intptr_t)(hz << 16 | ms), 4, NULL) != pdPASS) toning = false;   // play converts through a 1 KB buffer on this stack
}

void speaker_play_end(void)
{
    if (!dac || !enabled) return;
    uint8_t quiet[DMA_BUF];
    memset(quiet, 128, sizeof quiet);              // mid-scale = silence into an AC-coupled amplifier
    size_t loaded = 0;
    dac_continuous_write(dac, quiet, sizeof quiet, &loaded, 200);
    vTaskDelay(pdMS_TO_TICKS(DMA_BUFS * DMA_BUF * 1000 / SPEAKER_RATE + 20));   // let the last buffers play out
    sleep_dac();
}

int speaker_volume(void)
{
    int v = settings.volume;
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

void speaker_set_volume(int level)
{
    settings.volume = level < 0 ? 0 : level > 100 ? 100 : level;
    settings_save();
}
