// shell.c - the little OS (see shell.h). It owns the keyboard (one reader, so
// nothing races the FIFO) and shows a tabbed launcher: Left/Right page between
// tabs (APPS / UTILS / PREFS / SAPPHIRE), Up/Down move the row, Select launches,
// Back returns. App #1 is the Walkie-Talkie; the rest are placeholders until
// their phases wire them in.
#include "shell.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "board.h"
#include "screen.h"
#include "walkie.h"

static const char *TAG = "shell";

// navigation keys (mapped on-metal with the console `kbd` dump; Ctrl = 4 is PTT).
// UP/DOWN/SELECT/BACK are confirmed; LEFT/RIGHT are set from a kbd dump before flashing.
#define KEY_UP      57
#define KEY_DOWN    58
#define KEY_LEFT    56           // TODO(kbd): confirm the left-arrow key number
#define KEY_RIGHT   59           // TODO(kbd): confirm the right-arrow key number
#define KEY_SELECT  67
#define KEY_BACK    65

// Apps: a launchable screen, grouped into tabs below.
enum { APP_WALKIE, APP_IR, APP_TERMINAL, APP_SD, APP_BRIGHT, APP_VOLUME, APP_BACKUP, APP_N };
static const char *const APP_NAME[APP_N] = {
    "WALKIE-TALKIE", "IR BLASTER", "TERMINAL", "SD CARD", "BRIGHTNESS", "VOLUME", "BACKUP",
};

// Tabs: a name and the apps it holds. One page at a time; Left/Right page.
static const uint8_t APPS_TAB[]  = { APP_WALKIE, APP_IR };
static const uint8_t UTILS_TAB[] = { APP_TERMINAL, APP_SD };
static const uint8_t PREFS_TAB[] = { APP_BRIGHT, APP_VOLUME };
static const uint8_t SAPPH_TAB[] = { APP_BACKUP };
static const struct { const char *name; const uint8_t *apps; uint8_t n; } TABS[] = {
    { "APPS",     APPS_TAB,  2 },
    { "UTILS",    UTILS_TAB, 2 },
    { "PREFS",    PREFS_TAB, 2 },
    { "SAPPHIRE", SAPPH_TAB, 1 },
};
#define NTAB ((int)(sizeof TABS / sizeof TABS[0]))

static int tab;                          // current tab (page)
static int sel;                          // selection within the tab
static int current = -1;                 // -1 = launcher, else the running app id

static void show_launcher(void)
{
    const char *items[8];
    int n = TABS[tab].n;
    for (int i = 0; i < n; i++) items[i] = APP_NAME[TABS[tab].apps[i]];
    screen_tabs(TABS[tab].name, items, n, sel, tab, NTAB);
}

// Status cues (idle/nolink from the network, listen/think/speak from Walkie)
// funnel through here so they only paint while the Walkie app owns the screen -
// otherwise a WiFi event would clobber the launcher.
void shell_cue(const char *name)
{
    if (!name) return;
    if (current == APP_WALKIE) screen_status(name);
}

static void launch(int app)
{
    if (app == APP_WALKIE) { current = APP_WALKIE; walkie_enter(); }
    else { ESP_LOGI(TAG, "%s: coming soon", APP_NAME[app]); }   // placeholder: stay on the launcher
}

static void leave(void)
{
    if (current == APP_WALKIE) walkie_exit();
    current = -1;
    show_launcher();
}

static void on_key(int key, bool down)
{
    if (current < 0) {                   // launcher: act on key-down
        if (!down) return;
        int n = TABS[tab].n;
        if      (key == KEY_UP)     { sel = (sel + n - 1) % n; show_launcher(); }
        else if (key == KEY_DOWN)   { sel = (sel + 1) % n; show_launcher(); }
        else if (key == KEY_LEFT)   { tab = (tab + NTAB - 1) % NTAB; sel = 0; show_launcher(); }
        else if (key == KEY_RIGHT)  { tab = (tab + 1) % NTAB; sel = 0; show_launcher(); }
        else if (key == KEY_SELECT) launch(TABS[tab].apps[sel]);
        return;
    }
    if (key == KEY_BACK && down) { leave(); return; }
    if (current == APP_WALKIE) walkie_key(key, down);
}

static void shell_task(void *arg)
{
    board_kbd_init();
    show_launcher();
    for (;;) {
        int key; bool down;
        while (!board_kbd_busy() && board_kbd_read(&key, &down)) on_key(key, down);
        if (current == APP_WALKIE) walkie_tick();
        bool fast = current == APP_WALKIE && walkie_recording();
        vTaskDelay(pdMS_TO_TICKS(fast ? 2 : 20));
    }
}

void shell_start(void)
{
    static bool started;
    if (started) return;
    started = true;
    xTaskCreate(shell_task, "shell", 8192, NULL, 5, NULL);   // 8 KB: walkie_tick runs the voice TLS
    ESP_LOGI(TAG, "shell up: tabbed launcher + walkie-talkie");
}
