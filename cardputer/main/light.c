// light.c - the board's status surface. On the Cardputer that surface is the
// screen: light_start brings the home page up, light_cue redraws the status
// word. The cue names (idle/listen/think/speak/nolink) come from net/link/ptt.
#include "light.h"

#include "esp_log.h"
#include "screen.h"
#include "shell.h"

static const char *TAG = "light";

void light_start(void) { screen_start(); }

// Cues come from the network (net/link) and from apps. The shell decides
// whether they paint, so a WiFi event never overwrites the launcher.
void light_cue(const char *name)
{
    ESP_LOGD(TAG, "cue %s", name ? name : "?");
    shell_cue(name);
}

void light_speaking(bool on) { shell_cue(on ? "speak" : "idle"); }
