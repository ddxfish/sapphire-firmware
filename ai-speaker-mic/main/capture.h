// capture.h - the room, in pieces, to Sapphire (0.7.6). While a capture goes on, every `chunk_s` seconds of
// the clean mic stream is posted to POST /api/devices/<name>/room from its own task, so the web server stays
// free and the board keeps hearing its wake word. A fire meanwhile is stamped into the chunk it fell in, with
// the model's score. In test mode a fire is only counted, nothing is recorded or sent: a candidate model can
// be judged on a day of the room without her answering the television.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

void capture_init(void);                       // once, at start: the sender task
// Begins, for `seconds` (1 to 86400), in pieces of `chunk_s` seconds (5 to 60). False, with `why`, when a
// capture is going on, no sound is coming in, or the memory for two pieces is not there.
bool capture_start(int seconds, int chunk_s, bool test, char *why, size_t why_len);
void capture_stop(void);                       // what was gathered of the last piece is sent, then the buffers go
bool capture_active(void);
bool capture_test(void);                       // a capture goes on and its fires are not to be acted on
void capture_feed(const int16_t *pcm, size_t samples);   // from the audio sink: every chunk of clean audio
void capture_fire(float score);                // the wake word was heard; from the audio sink, before anything else
cJSON *capture_json(void);                     // for /health and GET /capture
