// audio.h - sound both ways, at one rate, with no codec chip.
//
//   in:  the PDM mic (I2S0) -> digital gain -> Espressif's AFE fed as "MR": the mic
//        and, beside it, what the speaker is playing (the reference the echo
//        canceller needs; this board has no loopback wire, so it is made here)
//        -> noise suppression, voice detection -> one clean 16 kHz mono stream -> the sink (wake.c)
//   out: 16 kHz mono PCM -> volume -> I2S1 stereo -> the NS4168 amplifier. One player at a time.
//
// The reference (the R&D knob, 2026-11-02): every sample written to the amplifier is also
// written into a ring, and read out of it `aec_delay_ms` later, when its echo is due at the
// mic (the amplifier's DMA depth, the air, the mic's own DMA). The canceller's filter spans
// a little slack after that; a reference BEHIND the echo cancels nothing. `aec measure` on
// the console plays a burst and times its return, so the delay is read off the board itself.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#define AUDIO_RATE 16000               // the mic, the front end, the wake word, what goes to Sapphire
#define SPEAKER_RATE 24000             // the default for her voice: Kokoro makes 24 kHz and the amplifier's clock is its own; the reference is decimated to 16. settings.speaker_rate picks 16000 or 24000 live

// Called on the pipeline's own task with each cleaned chunk. Be quick.
typedef void (*audio_sink_fn)(const int16_t *pcm, size_t samples, bool speech);

void audio_start(void);                  // after board_init (main.c): the AFE and its two tasks
// A task whose stack lives in PSRAM (fine for one that never writes flash):
// internal RAM is the scarce thing on this board, and TLS needs it.
void task_in_psram(TaskFunction_t fn, const char *name, size_t stack, int priority, int core);
void audio_stop(void);
bool audio_ok(void);
void audio_set_sink(audio_sink_fn fn);
void audio_suspend(bool on);             // sleep: both I2S channels stopped, nothing fed, nothing played; false brings them back

bool audio_take_speaker(TickType_t wait);
void audio_give_speaker(void);
void audio_play(const int16_t *pcm, size_t samples, int channels);   // holding the speaker; blocks. SPEAKER_RATE samples
void audio_play_end(void);
void audio_tone(int hz, int ms);                                       // takes the speaker itself
void audio_set_volume(int level);                                      // 0 to 100, kept in flash
int audio_volume(void);
void audio_set_mic_gain(float db);        // 0 to 30, digital, live and saved
float audio_mic_gain(void);
void audio_set_mute(bool on);             // the amplifier unpowered; the reference goes quiet with it
bool audio_muted(void);

// --- the echo reference ------------------------------------------------------------
void audio_aec_set_delay_ms(int ms);      // 0 to 500, kept in flash, live
int audio_aec_delay_ms(void);
void audio_aec_enable(bool on);           // off: the canceller is fed silence as its reference (hears the echo whole)
bool audio_aec_enabled(void);
// Plays a short burst and listens for it on the raw mic: `delay_ms` is when it came back
// (push to mic), `ratio` how far above the room it stood (under 3: nothing was heard).
// Blocks about a second. False when no sound runs or the speaker is busy.
bool audio_aec_measure(int *delay_ms, float *ratio);
void audio_stats(float *out_rms, int *speech_ms);
float audio_mic_rms(void);
float audio_mic_hz(void);
float audio_spk_hz(void);                 // the amplifier's real frame rate against the timer (0 until it is known)
int audio_ref_ppm(void);
void audio_ref_ratio(bool on);           // bench: resample the reference by the measured clocks (default) or the nominal 2:3
bool audio_ref_ratio_on(void);                  // the reference's resampling correction in ppm (0 = nominal, the rates not known yet)
void audio_mic_health(int *dropped_ms, int *feed_max_ms, int *feed_slow, int *pops_stale);   // the mic ring's losses and the feed's worst case                 // the mic's real sample rate against the timer (0 until it is known)                // the raw (gained) mic over the last chunk: with the output, the echo return loss while she speaks
int audio_speaker_rate(void);             // 16000 or 24000
bool audio_set_speaker_rate(int rate);    // live: the amplifier's clock is set over, kept in flash; Sapphire reads plays.rate again within a minute (its health cache)   // the front end's output: its level over the last chunk, and how much of the last 2 s the VAD called speech

typedef struct {
    bool codec_ok;
    float quiet_rms;                   // the mic, the room
    float tone_rms;                    // the mic, during a tone from the speaker
    float slot_rms[4];                 // every channel fed during the tone: mic, reference
    float quiet_slots[4];              // every channel fed in quiet
    float out_quiet;                   // the front end's output in quiet
    float out_tone;                    // ... during the tone's second half: what the wake word hears of the speaker (the canceller's work)
} audio_selftest_t;
void audio_selftest(audio_selftest_t *out);
