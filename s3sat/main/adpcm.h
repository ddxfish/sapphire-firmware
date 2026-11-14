// adpcm.h - IMA ADPCM, mono, the WAV block shape (Sapphire's core/devices/audio_codecs.py makes it).
// Header only. A block is: int16 first sample, u8 step index, u8 0, then two samples a byte, the low
// nibble first; every block starts afresh, so a run of blocks decodes from anywhere a block begins.
// The last block may be short: it holds 1 + 2 x (its bytes - 4) samples, cut to `samples`.
// The reference this mirrors is ima_decode() in Sapphire's tests/test_avatar_feed.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

static const int16_t ADPCM_STEPS[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
static const int8_t ADPCM_MOVES[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

// Samples `bytes` of blocks of `block` bytes hold (the last may be short).
static inline size_t adpcm_samples(size_t bytes, size_t block)
{
    if (block < 5) return 0;
    size_t whole = bytes / block, rest = bytes % block;
    return whole * (1 + 2 * (block - 4)) + (rest >= 4 ? 1 + 2 * (rest - 4) : 0);
}

// Decodes up to `max` samples into `out`. Returns how many came out.
static inline size_t adpcm_decode(const uint8_t *in, size_t bytes, size_t block, int16_t *out, size_t max)
{
    size_t made = 0;
    if (block < 5) return 0;
    for (size_t at = 0; at + 4 <= bytes && made < max; at += block) {
        const uint8_t *b = in + at;
        size_t end = at + block < bytes ? block : bytes - at;
        int pred = (int16_t)(b[0] | (b[1] << 8));
        int idx = b[2] > 88 ? 88 : b[2];
        out[made++] = (int16_t)pred;
        for (size_t i = 4; i < end && made < max; i++) {
            for (int half = 0; half < 2 && made < max; half++) {
                int nib = half ? b[i] >> 4 : b[i] & 15;
                int step = ADPCM_STEPS[idx], diff = step >> 3;
                if (nib & 4) diff += step;
                if (nib & 2) diff += step >> 1;
                if (nib & 1) diff += step >> 2;
                pred += nib & 8 ? -diff : diff;
                pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
                idx += ADPCM_MOVES[nib & 7];
                idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
                out[made++] = (int16_t)pred;
            }
        }
    }
    return made;
}
