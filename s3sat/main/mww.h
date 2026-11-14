// mww.h - a microWakeWord model on clean 16 kHz mono audio. The models are
// the open ones from esphome/micro-wake-word-models (version 2): 40 log-mel
// features every 10 ms into a small streaming network that answers one
// probability. A wake is a run of high probabilities.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *model;              // the .tflite, in flash
    size_t model_size;
    float probability_cutoff;          // from the model's manifest
    int sliding_window_size;
    int tensor_arena_size;
    int feature_step_ms;
} mww_config_t;

bool mww_init(const mww_config_t *cfg);
bool mww_feed(const int16_t *pcm, size_t samples);    // true the moment the word is heard
void mww_reset(void);                                  // after a wake, or a pause
float mww_last(void);                                  // the last window's average probability, 0 to 1 (what a fire scored)

#ifdef __cplusplus
}
#endif
