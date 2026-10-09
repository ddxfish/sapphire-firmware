// audio.h - sound both ways, at one rate.
//
//   in:  the codec's four slots -> Espressif's AFE (echo cancelling, noise,
//        voice detection) -> one clean 16 kHz mono stream -> the sink (wake.c)
//   out: 16 kHz mono PCM -> the codec. One player at a time.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#define AUDIO_RATE 16000

// Called on the pipeline's own task with each cleaned chunk. Be quick.
typedef void (*audio_sink_fn)(const int16_t *pcm, size_t samples, bool speech);

void audio_start(void);                  // after board_init (main.c): the AFE and its two tasks
// A task whose stack lives in PSRAM (fine for one that never writes flash):
// internal RAM is the scarce thing on this board, and TLS needs it.
void task_in_psram(TaskFunction_t fn, const char *name, size_t stack, int priority, int core);
void audio_stop(void);
bool audio_ok(void);
void audio_set_sink(audio_sink_fn fn);

bool audio_take_speaker(TickType_t wait);
void audio_give_speaker(void);
void audio_play(const int16_t *pcm, size_t samples, int channels);   // holding the speaker; blocks
void audio_play_end(void);
void audio_tone(int hz, int ms);                                       // takes the speaker itself
void audio_set_volume(int level);
void audio_set_mic_gain(float db);        // 0 to 37.5, live and saved
float audio_mic_gain(void);                                      // 0 to 100, kept in flash
int audio_volume(void);
void audio_set_mute(bool on);             // the amplifier off; sound still flows to the codec, so echo cancelling keeps its reference
bool audio_muted(void);

typedef struct {
    bool codec_ok;
    float quiet_rms;                   // slot 0, the room
    float tone_rms;                    // slot 0, during a tone from the speaker
    float slot_rms[4];                 // every channel read during the tone: finds the echo
    float quiet_slots[4];              // every channel read in quiet: a loopback reads about nothing
} audio_selftest_t;
void audio_selftest(audio_selftest_t *out);
