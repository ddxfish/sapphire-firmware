// wav.h - the 44 byte header of a 16 bit PCM WAV, both ways; and reading an IMA ADPCM one (0.6.0). Header only.
#pragma once
#include <stdint.h>
#include <string.h>

#define WAV_HEADER 44

static inline void wav_put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline void wav_put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline uint32_t wav_get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint16_t wav_get16(const uint8_t *p) { return p[0] | (p[1] << 8); }

// A header for `bytes` of 16 bit PCM at `rate` with `channels`.
static inline void wav_header(uint8_t out[WAV_HEADER], uint32_t rate, uint16_t channels, uint32_t bytes)
{
    memcpy(out, "RIFF", 4); wav_put32(out + 4, 36 + bytes); memcpy(out + 8, "WAVEfmt ", 8);
    wav_put32(out + 16, 16); wav_put16(out + 20, 1); wav_put16(out + 22, channels);
    wav_put32(out + 24, rate); wav_put32(out + 28, rate * channels * 2);
    wav_put16(out + 32, channels * 2); wav_put16(out + 34, 16);
    memcpy(out + 36, "data", 4); wav_put32(out + 40, bytes);
}

#define WAV_PCM 1
#define WAV_IMA 0x11
typedef struct { uint32_t rate; uint16_t channels; uint16_t bits; uint32_t data_at;
                 uint16_t format, block; uint32_t samples; } wav_info_t;   // block, samples: IMA only (block align; the fact chunk's count, 0 when absent)

// Reads the header at the start of a WAV. Returns 0 on a shape it cannot
// play: 16 bit PCM, mono or stereo, or IMA ADPCM mono (4 bits, its block align
// read from fmt). `have` is how many bytes of the file are in `p` (64 or more
// is enough for any sane header; a LIST chunk before the data is skipped).
static inline int wav_parse(const uint8_t *p, size_t have, wav_info_t *out)
{
    if (have < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return 0;
    size_t at = 12;
    int fmt = 0;
    out->samples = 0;
    while (at + 8 <= have) {
        uint32_t size = wav_get32(p + at + 4);
        if (!memcmp(p + at, "fmt ", 4) && at + 8 + 16 <= have) {
            out->format = wav_get16(p + at + 8);
            if (out->format != WAV_PCM && out->format != WAV_IMA) return 0;
            out->channels = wav_get16(p + at + 10);
            out->rate = wav_get32(p + at + 12);
            out->block = wav_get16(p + at + 20);
            out->bits = wav_get16(p + at + 22);
            fmt = 1;
        } else if (!memcmp(p + at, "fact", 4) && at + 12 <= have) {
            out->samples = wav_get32(p + at + 8);
        } else if (!memcmp(p + at, "data", 4)) {
            out->data_at = at + 8;
            if (!fmt) return 0;
            if (out->format == WAV_IMA) return out->bits == 4 && out->channels == 1 && out->block >= 5;
            return out->bits == 16 && out->channels >= 1 && out->channels <= 2;
        }
        if (size > have - at - 8) return 0;              // a chunk that runs past what is here (a size near 4 GB made `at` stand still, for ever)
        at += 8 + size + (size & 1);
    }
    return 0;
}
