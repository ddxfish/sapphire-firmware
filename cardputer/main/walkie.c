// walkie.c - the Walkie-Talkie app (see walkie.h). Hold Ctrl to talk: stream
// 16 kHz mono to Sapphire while held, finish on release. Driven by the shell
// (no task of its own) - the shell calls walkie_key on Ctrl events and
// walkie_tick every loop while this app is the active one. 60 s safety stop.
#include "walkie.h"

#include "esp_log.h"

#include "board.h"
#include "light.h"
#include "link.h"

static const char *TAG = "walkie";
#define PTT_KEY       4          // Ctrl, lower-left
#define CHUNK         1600       // 100 ms at 16 kHz
#define SAFETY_MS     60000      // a stuck key cannot stream forever

static int16_t chunk[CHUNK];
static bool recording;           // Ctrl held
static bool streaming;           // a voice stream is open
static int ms;

void walkie_enter(void)
{
    recording = false;
    streaming = false;
    ms = 0;
    light_cue(link_connected() ? "idle" : "nolink");   // paint the walkie home page
}

void walkie_exit(void)
{
    if (streaming) { char h[4]; link_voice_end(h, sizeof h); streaming = false; }
    recording = false;
}

bool walkie_recording(void) { return recording; }

void walkie_key(int code, bool down)
{
    if (code != PTT_KEY) return;
    if (down && !recording) {
        if (link_voice_begin()) { recording = true; streaming = true; ms = 0; light_cue("listen"); }
    } else if (!down) {
        recording = false;
    }
}

void walkie_tick(void)
{
    if (recording) {
        int got = board_mic_read(chunk, CHUNK);
        if (got > 0) {
            if (!link_voice_write(chunk, got)) recording = false;
            else ms += got * 1000 / AUDIO_RATE;
        }
        if (ms >= SAFETY_MS) recording = false;
    }
    if (!recording && streaming) {
        light_cue("think");
        char heard[256] = {0};
        bool ok = link_voice_end(heard, sizeof heard);
        ESP_LOGI(TAG, "PTT %d ms -> %s: \"%s\"", ms, ok ? "sent" : "lost", heard);
        if (!link_connected()) light_cue("nolink");   // else leave THINKING; the events stream drives it from here
        streaming = false;
    }
}
