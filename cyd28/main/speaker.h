// speaker.h - her voice through the board's amplifier, from the chip's own
// 8-bit DAC (GPIO 26 on this board, DAC channel 1), DMA-fed.
//
// The protocol's `speaker` without a `mic`: /health says `plays` (16-bit
// mono WAV at SPEAKER_RATE) and Sapphire fits each sentence to that before
// sending. server.c streams the body in and hands 16-bit frames here; this
// file turns them into 8-bit, scaled by the volume, and keeps the DMA fed.
// Between sentences the DAC idles at mid-scale, so the amplifier hears
// silence, not a step.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool speaker_start(void);                              // the DAC channel; false = no speaker
bool speaker_ok(void);
void speaker_play(const int16_t *frames, size_t samples, int channels);   // blocks while the DMA is full
void speaker_play_end(void);                           // the tail: drain, fade to silence
int speaker_volume(void);                              // 0..100
void speaker_set_volume(int level);                    // kept in flash
void speaker_tone(int hz, int ms);                     // a plain sine, on a task of its own: a check from the Board page
