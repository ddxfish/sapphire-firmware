// ir.c - IR blaster (see ir.h). The onboard emitter is on GPIO44; we drive it
// with the RMT peripheral at a 1 us tick and a 38 kHz carrier on the marks.
// Phase 1 sends standard NEC frames; a raw-symbol path (for learned codes and
// other protocols) slots in later.
#include "ir.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"

static const char *TAG = "ir";
#define IR_GPIO 44

static rmt_channel_handle_t chan;
static rmt_encoder_handle_t enc;

bool ir_init(void)
{
    if (chan) return true;
    rmt_tx_channel_config_t cfg = {
        .gpio_num = IR_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,          // 1 us per tick, so durations below are microseconds
        .mem_block_symbols = 64,           // one NEC frame is ~34 symbols
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&cfg, &chan) != ESP_OK) { ESP_LOGE(TAG, "no RMT channel"); return false; }
    rmt_carrier_config_t carrier = { .frequency_hz = 38000, .duty_cycle = 0.33 };   // 38 kHz on the marks
    rmt_apply_carrier(chan, &carrier);
    rmt_copy_encoder_config_t ec = {};
    if (rmt_new_copy_encoder(&ec, &enc) != ESP_OK) { ESP_LOGE(TAG, "no encoder"); return false; }
    rmt_enable(chan);
    ESP_LOGI(TAG, "IR blaster up on GPIO%d", IR_GPIO);
    return true;
}

// a mark (carrier on) of `mark` us then a space (off) of `space` us
static rmt_symbol_word_t sym(uint16_t mark, uint16_t space)
{
    return (rmt_symbol_word_t){ .level0 = 1, .duration0 = mark, .level1 = 0, .duration1 = space };
}

void ir_send_nec(uint16_t addr, uint8_t cmd)
{
    if (!chan) return;
    rmt_symbol_word_t s[34];
    int n = 0;
    s[n++] = sym(9000, 4500);              // NEC leader: 9 ms mark, 4.5 ms space
    uint8_t a_lo = addr & 0xff;
    uint8_t a_hi = addr > 0xff ? (uint8_t)(addr >> 8) : (uint8_t)~a_lo;   // extended keeps the high byte; standard is its complement
    uint32_t frame = (uint32_t)a_lo | ((uint32_t)a_hi << 8)
                   | ((uint32_t)cmd << 16) | ((uint32_t)(uint8_t)~cmd << 24);
    for (int i = 0; i < 32; i++)           // 32 bits, LSB first; a 1 is a long space, a 0 a short one
        s[n++] = (frame & (1u << i)) ? sym(560, 1690) : sym(560, 560);
    s[n++] = sym(560, 0);                  // trailing mark
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(chan, enc, s, n * sizeof s[0], &tx);
    rmt_tx_wait_all_done(chan, 100);
}
