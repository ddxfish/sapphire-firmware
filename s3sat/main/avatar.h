// avatar.h - her face and her voice as ONE stream: packets of audio and frames on one clock.
//
// A packet is ~0.96 s of 16-bit mono PCM at the speaker's rate plus the frames due within
// it, each a JPEG of the whole face (a key) or of the region that changed, with the moment
// it is due. The player feeds the audio to the speaker and paints each frame when the audio
// reaches its moment: the samples fed are the clock, so voice and face can only ever be what
// the packet says, and the stream ends when the audio ends. The face server's own browser
// viewer works this way; the board did not until 0.2.0, and her voice (a plain WAV from
// Sapphire) and her face (from the server's render pipeline) drifted apart however they were
// glued (2026-11-03).
//
// A packet with no audio is her idle (0.3.0): its frames are paced on the wall over the span
// the head states ("ms"). The speaker is taken only while packets carry her voice and goes
// back at the first packet without it; her idle neither opens her tile nor wakes the screen.
//
//   v1: <4 bytes big-endian: head length> <head JSON> <audio bytes> <frame bytes, in order>
//       head: {"kind": "speech"|"idle", "sr": 24000, "audio": <bytes>, "ms": <the packet's span>,
//              "frames": [{"t": <ms into the packet>, "x","y","w","h": <multiples of 16>, "n": <bytes>, "key": bool}, ...]}
//   v2 (0.6.0, /health screen.av_v 2): <head length> <head JSON, padded to 4> <nf x 16-byte frame entries> <audio> <frames>
//       head: {"v": 2, "kind", "sr", "codec": "pcm"|"ima_adpcm", "audio": <bytes>, "samples", "block", "ms", "nf"}
//       entry, big-endian: t u16 (ms), x u16, y u16, w u16, h u16, flags u16 (bit 0: key), n u32 (bytes)
//   No JSON node a frame: a packet of 96 frames parsed as JSON took ~45 KB of internal heap (cJSON's
//   small mallocs stay internal) against ~52 KB free. Either shape may carry "codec": "ima_adpcm"
//   with "samples" and "block": the audio is then IMA blocks (adpcm.h), decoded into PSRAM on arrival.
//   Several entries may share a "t": a frame that goes as the few boxes that changed.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"

#define AV_QUEUE 3                     // packets held: about three seconds of her
#define AV_FRAMES_MAX 96               // frame entries a packet may carry (/health screen.av_frames): 25 frames a second, up to four boxes each, within reason
#define AV_W BOARD_AV_W                // the box her face fills (/health screen.av_w, av_h)
#define AV_H BOARD_AV_H

void avatar_start(void);               // the queue and the player task
bool avatar_ok(void);                  // it started: packets can be taken (health screen.av)
// A packet arrived (the door hands over its body; this takes the buffer and frees it when played).
// Returns 0 = taken, 429 = the queue is full, 400 = not a packet, 415 = the audio's rate is not the speaker's,
// 204 = it carried her voice while `refuse_voice` (the board is stopped, or hears a question): dropped, not played.
int avatar_take(uint8_t *body, size_t len, int *queued, bool refuse_voice);
void avatar_clear(void);               // a stop: what is queued is dropped, the player goes quiet
bool avatar_live(void);                // a packet in the last 3 s, or one playing
bool avatar_voice(void);               // her voice is playing from her stream, or a packet of it waits to: her turn
int avatar_scratch_mod16(void);        // the decode buffer's address mod 16: must be 0 (health screen.av_stream.scratch_mod16)
bool avatar_need_key(void);            // a frame did not paint since the last ask: the next answer says need_key (once)
typedef struct { int packets, underruns, frames, late, skipped, queued, decode_ms, gap_ms, paint_ms, decode_fail, paint_fail;
                 int fail_step, fail_w, fail_h, fail_n, hdr_w, hdr_h, err; bool playing, voice; } avatar_stats_t;   // the last decode failure: which step, the frame, what the header said, the decoder's error
void avatar_stats(avatar_stats_t *out);
