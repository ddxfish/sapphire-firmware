// wake.h - listens on the clean mic stream for the wake word, then records
// the question until the speaker stops and hands it to Sapphire. Also the
// on-demand recording behind GET /audio/listen.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t *pcm;                      // 16 kHz mono, owned by wake.c until wake_record_done
    size_t samples;
} recording_t;

void wake_start(void);
// Another device, or Sapphire's own microphone, took this wake: a recording
// in progress ends now and nothing is sent. Safe to call at any time.
void wake_standdown(const char *who);
bool wake_enabled(void);
void wake_enable(bool on);
void wake_pause(bool on);              // a backup is landing: no wake word, no question by hand, until false
bool wake_paused(void);
void wake_trigger(void);               // as if the word was heard, even with the wake word off: the glass's middle
bool wake_running(void);               // the model is loaded and audio flows
bool wake_busy(void);                  // a question is being recorded or sent: the card transfer waits
const char *wake_model_name(void);
float wake_model_threshold(void);
bool wake_model_stored(void);          // a model from PUT /wakeword/model is the one listening
bool wake_model_store(const uint8_t *data, size_t size, const char *name, const char *phrase, float threshold,
                      int sliding_window, int step_ms, char *why, size_t why_len);
bool wake_model_forget(void);          // back to the embedded model at the next start
// Records now, without a wake word: with use_vad, until the speaker stops
// (up to max_s seconds); without, the room for max_s seconds flat. Blocks.
// False when a recording is already going on.
bool wake_record_now(int max_s, bool use_vad, recording_t *out);
void wake_record_done(recording_t *rec);
