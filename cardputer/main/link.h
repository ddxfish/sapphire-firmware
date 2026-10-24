// link.h - the board's side of Sapphire's device doors. P1 holds the events
// stream open (GET /api/devices/<name>/events) so the board registers and
// shows in Settings > Devices. Voice / room / press arrive in later phases.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void link_start(void);          // once WiFi is up: holds the events stream open
bool link_connected(void);      // the events stream is open and Sapphire greeted us

// Send a 16 kHz mono recording to Sapphire's voice door (POST /voice). Returns
// true when she took it; `heard` gets her transcript (may be empty on silence).
bool link_send_voice(const int16_t *pcm, size_t samples, char *heard, size_t heard_size);

// Streaming voice (push-to-talk): open the POST on key-down, write 16 kHz mono
// as it is captured, finish on key-up. No length cap and a few KB of RAM - the
// WAV is sent with a sentinel length and the body is chunked. One at a time.
bool link_voice_begin(void);                              // open the stream; false if it could not
bool link_voice_write(const int16_t *pcm, size_t samples);// append a piece; false once the stream has failed
bool link_voice_end(char *heard, size_t heard_size);      // finish, read her transcript; always closes
