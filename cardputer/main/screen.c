// screen.c - ST7789V2 240x135 UI (see screen.h). The panel's GRAM is wired
// portrait (135 columns x 240 rows); the Cardputer is held landscape, so we
// draw in plain landscape (x,y) and let px() map each pixel into the panel's
// native frame (transpose + flip + byte-swap, the panel reads big-endian).
//
// No framebuffer. The board has no PSRAM, and once WiFi/HTTP/voice are up the
// internal heap is too fragmented for a 63 KB contiguous buffer - so a paint
// would just fail. Instead we render in native-row strips straight into the
// one static blit buffer: render_scene() walks the panel a strip at a time and
// re-runs the draw for each, and px() keeps only the pixels that land in the
// current strip. Screen RAM is fixed at ~6.5 KB and a paint can never run out.
// The only board-specific unknowns are two flip flags, tunable live over the
// console (`screen cfg`); the gap is the glass's offset within the GRAM.
#include "screen.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "font8x8.h"

static const char *TAG = "screen";

// --- panel wiring (M5 Cardputer ADV; same display as the original Cardputer) ---
#define LCD_SCLK   36
#define LCD_MOSI   35
#define LCD_CS     37
#define LCD_DC     34
#define LCD_RST    33
#define LCD_BL     38            // backlight: PWM, ACTIVE HIGH
#define LCD_HOST   SPI3_HOST
#define LCD_HZ     (40 * 1000 * 1000)

#define SCREEN_W   240           // landscape logical width
#define SCREEN_H   135           // landscape logical height
#define NCOL       135           // panel native columns (CASET)
#define NROW       240           // panel native rows (RASET)
#define GAP_X      52            // the 135-wide glass sits at column 52 in GRAM
#define GAP_Y      40            // the 240-tall glass sits at row 40 in GRAM
#define CHUNK_ROWS 24            // native rows per strip (fits the line buffer)

// RGB565 (natural; byte-swapped by px when it writes the strip)
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GRAY   0x8410
#define C_GREEN  0x07E0
#define C_YELLOW 0xFFE0
#define C_CYAN   0x07FF
#define C_RED    0xF800
#define C_BLUE   0x5D9F
#define C_HL     0x3186          // dim blue: the selected menu row

#define TITLE_Y  14
#define SUB_Y    46
#define STATUS_Y 96

static esp_lcd_panel_handle_t panel;
static bool ready;
static int flip_x = 0, flip_y = 1;             // tuned on glass (console `screen cfg`)
static int gap_x = GAP_X, gap_y = GAP_Y;       // 52 / 40: the standard Cardputer CASET/RASET offset
static uint16_t line[NCOL * CHUNK_ROWS];       // one native strip, byte-swapped, blitted as-is
static int strip_nr, strip_rows;               // native-row range of the strip px() is filling
static SemaphoreHandle_t paint_lock;           // one painter at a time (cues come from several tasks)
static SemaphoreHandle_t blit_done;            // given from ISR when a strip's DMA finishes

// esp_lcd's draw_bitmap is async: it queues the SPI/DMA transfer and returns.
// We reuse one `line` buffer for every strip, so we MUST wait for each strip's
// transfer to finish before refilling the buffer - otherwise the DMA reads a
// half-rewritten buffer and the panel shows shredded vertical bands.
static bool IRAM_ATTR on_blit_done(esp_lcd_panel_io_handle_t io,
                                   esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    (void)io; (void)e; (void)ctx;
    BaseType_t hp = pdFALSE;
    if (blit_done) xSemaphoreGiveFromISR(blit_done, &hp);
    return hp == pdTRUE;
}

static void blit(int x0, int y0, int x1, int y1)
{
    esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, line);
    if (blit_done) xSemaphoreTake(blit_done, pdMS_TO_TICKS(100));   // buffer is free once DMA is done
}

// --- drawing in landscape coords; px routes each pixel into the current strip ---
// Landscape (x,y) -> native (row,col): row = x (flipped), col = y (flipped). A
// pixel outside the strip being built is dropped; the rest is stored at the
// panel's byte order so the strip blits with no further pass.
static void px(int x, int y, uint16_t color)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    int nrow = flip_x ? (NROW - 1 - x) : x;    // native row for this landscape x
    int rr = nrow - strip_nr;
    if (rr < 0 || rr >= strip_rows) return;    // not in the strip being built
    int nc = flip_y ? (NCOL - 1 - y) : y;      // native column for this landscape y
    line[rr * NCOL + nc] = __builtin_bswap16(color);
}

static void box(int x, int y, int w, int h, uint16_t color)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) px(xx, yy, color);
}

// A small filled triangle arrow (sz x sz) for the tab pager: tip at the vertical
// centre, pointing left or right. The font has no '<'/'>' glyphs, so we draw it.
static void arrow(int x, int y, int sz, bool left, uint16_t color)
{
    for (int r = 0; r < sz; r++) {
        int d = r < sz / 2 ? sz / 2 - r : r - sz / 2;      // narrows to the tip at top/bottom
        for (int c = d; c < sz; c++)
            px(left ? x + c : x + sz - 1 - c, y + r, color);
    }
}

static void glyph(int gx, int gy, char ch, int scale, uint16_t color)
{
    if ((unsigned char)ch < FONT_FIRST || (unsigned char)ch > FONT_LAST) ch = ' ';
    const uint8_t *g = FONT8X8[(unsigned char)ch - FONT_FIRST];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if ((g[r] >> c) & 1)
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        px(gx + c * scale + sx, gy + r * scale + sy, color);
}

// Draw `text` left-aligned at (x, y). `adv` is the per-character advance.
static void text_at(int x, int y, const char *text, int scale, int adv, uint16_t fg)
{
    for (const char *p = text; *p; p++, x += adv) glyph(x, y, *p, scale, fg);
}

// Centre `text` on the band at y, clearing the band to bg first. `adv` is the
// per-character advance in pixels (8*scale is the natural width; a smaller adv
// tightens a long line so it clears the panel's ~18px edge clip).
static void text_line(int y, const char *text, int scale, int adv, uint16_t fg, uint16_t bg)
{
    box(0, y, SCREEN_W, 8 * scale, bg);
    int tw = (int)strlen(text) * adv;
    int x = (SCREEN_W - tw) / 2;
    if (x < 0) x = 0;
    text_at(x, y, text, scale, adv, fg);
}

// --- scenes: draw the whole landscape page; render_scene replays them per strip ---
static const char *s_word;                     // home/status page
static uint16_t s_col;
static const char *s_title;                    // menu page
static const char *const *s_items;
static int s_n, s_sel;
static int s_tab, s_ntab;                      // tab pager: which tab, how many

static void scene_page(void)
{
    text_line(TITLE_Y, "SAPPHIRE", 3, 24, C_BLUE, C_BLACK);       // 192 px, 24 px margin
    text_line(SUB_Y, "WALKIE-TALKIE", 2, 14, C_GRAY, C_BLACK);    // 182 px, tightened to clear the edge clip
    text_line(STATUS_Y, s_word, 3, 24, s_col, C_BLACK);
    box(SCREEN_W - 20, 5, 15, 15, s_col);                         // status "LED": a corner square in the state colour
}

static void scene_tabs(void)
{
    // the current tab's name, flanked by arrows; up to four rows; a dot per tab.
    text_line(6, s_title, 2, 14, C_CYAN, C_BLACK);
    arrow(8, 8, 12, true, C_GRAY);
    arrow(SCREEN_W - 20, 8, 12, false, C_GRAY);
    box(24, 26, SCREEN_W - 48, 2, C_GRAY);
    for (int i = 0; i < s_n && i < 4; i++) {
        int y = 34 + i * 22;
        if (i == s_sel) box(24, y - 3, SCREEN_W - 48, 20, C_HL);
        text_at(36, y, s_items[i], 2, 13, i == s_sel ? C_WHITE : C_GRAY);
    }
    int x0 = (SCREEN_W - s_ntab * 12) / 2;             // dot pager, centred
    for (int i = 0; i < s_ntab; i++)
        box(x0 + i * 12 + 3, SCREEN_H - 11, 6, 6, i == s_tab ? C_CYAN : C_GRAY);
}

static void scene_test(void)
{
    box(0, 0, SCREEN_W, 2, C_WHITE);                   // top
    box(0, SCREEN_H - 2, SCREEN_W, 2, C_WHITE);        // bottom
    box(0, 0, 2, SCREEN_H, C_WHITE);                   // left
    box(SCREEN_W - 2, 0, 2, SCREEN_H, C_WHITE);        // right
    box(0, 0, 24, 24, C_RED);                          // top-left    = RED
    box(SCREEN_W - 24, 0, 24, 24, C_GREEN);            // top-right   = GREEN
    box(0, SCREEN_H - 24, 24, 24, C_BLUE);             // bottom-left = BLUE
    text_line(STATUS_Y, "TEST", 3, 24, C_YELLOW, C_BLACK);
}

static void scene_ruler(void)
{
    box(0, 0, SCREEN_W, SCREEN_H, C_GRAY);
    box(0, 0, 8, SCREEN_H, C_WHITE);                   // left  (cam-visible)
    box(SCREEN_W - 8, 0, 8, SCREEN_H, C_GREEN);        // right
    box(0, 0, SCREEN_W, 8, C_CYAN);                    // top
    box(0, SCREEN_H - 8, SCREEN_W, 8, C_YELLOW);       // bottom
    for (int x = 0; x < SCREEN_W; x += 16) box(x, SCREEN_H / 2 - 4, 8, 8, C_BLACK);
}

// High-contrast calibration, white on black for a webcam on a 1" panel. One
// look answers: edges (a 3px white border flush to all four logical edges),
// every-pixel (no black bands should cut the border or text), centering (the
// cyan centre line + the centred text should sit on the middle), and letters
// (the alphabet + digits, whole and evenly spaced).
static void scene_cal(void)
{
    box(0, 0, SCREEN_W, 3, C_WHITE);                   // top edge
    box(0, SCREEN_H - 3, SCREEN_W, 3, C_WHITE);        // bottom edge
    box(0, 0, 3, SCREEN_H, C_WHITE);                   // left edge
    box(SCREEN_W - 3, 0, 3, SCREEN_H, C_WHITE);        // right edge
    box(SCREEN_W / 2 - 1, 0, 2, SCREEN_H, C_CYAN);     // true vertical centre
    text_line(14, "SAPPHIRE", 3, 24, C_WHITE, C_BLACK);
    text_line(52, "ABCDEFGHIJKLM", 2, 16, C_WHITE, C_BLACK);
    text_line(76, "NOPQRSTUVWXYZ", 2, 16, C_WHITE, C_BLACK);
    text_line(104, "0123456789", 2, 16, C_WHITE, C_BLACK);
}

// Walk the panel a strip at a time; each strip starts black, the scene draws
// into it (px keeps only this strip's pixels), then it blits. No big buffer.
static void render_scene(void (*scene)(void))
{
    if (!ready) return;
    for (int nr = 0; nr < NROW; nr += CHUNK_ROWS) {
        int rows = NROW - nr < CHUNK_ROWS ? NROW - nr : CHUNK_ROWS;
        strip_nr = nr; strip_rows = rows;
        memset(line, 0, (size_t)rows * NCOL * sizeof(uint16_t));   // black (bswap(0)=0)
        scene();
        blit(0, nr, NCOL, nr + rows);
    }
}

static bool paint(void (*scene)(void))
{
    if (!ready || !paint_lock) return false;
    if (xSemaphoreTake(paint_lock, pdMS_TO_TICKS(150)) != pdTRUE) return false;
    render_scene(scene);
    xSemaphoreGive(paint_lock);
    return true;
}

// Clear the whole controller RAM (240x320) to black once, so any pixels the
// glass shows outside our drawn window (panel-specific edge margins) are black
// rather than uninitialised garbage - kills the thin edge artefacts. Uses the
// strip buffer directly; call with no paint in flight (init, or lock held).
static void clear_gram(void)
{
    memset(line, 0, sizeof line);                      // black (bswap(0)=0)
    esp_lcd_panel_set_gap(panel, 0, 0);
    for (int x = 0; x < 240; x += NCOL)
        for (int y = 0; y < 320; y += CHUNK_ROWS) {
            int w = 240 - x < NCOL ? 240 - x : NCOL;
            int h = 320 - y < CHUNK_ROWS ? 320 - y : CHUNK_ROWS;
            blit(x, y, x + w, y + h);
        }
    esp_lcd_panel_set_gap(panel, gap_x, gap_y);
}

static void backlight(int percent)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_1, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1, .duty = percent * 255 / 100, .hpoint = 0,   // active high
    };
    ledc_channel_config(&c);
}

void screen_start(void)
{
    if (panel) return;
    spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK, .mosi_io_num = LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = NCOL * CHUNK_ROWS * 2,
    };
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { ESP_LOGE(TAG, "SPI did not start"); return; }
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_DC, .cs_gpio_num = LCD_CS, .pclk_hz = LCD_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io) != ESP_OK) return;
    blit_done = xSemaphoreCreateBinary();      // signalled from ISR when a strip's DMA completes
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_blit_done };
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL);
    esp_lcd_panel_dev_config_t cfg = {
        .reset_gpio_num = LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &cfg, &panel) != ESP_OK) { ESP_LOGE(TAG, "no ST7789 panel"); return; }
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);          // M5 panel is inverted
    esp_lcd_panel_set_gap(panel, gap_x, gap_y);        // the offset glass; no swap_xy/mirror (done in SW)
    esp_lcd_panel_disp_on_off(panel, true);
    backlight(90);
    paint_lock = xSemaphoreCreateMutex();
    ready = true;
    clear_gram();                                      // black out the edge margins first (no task yet)
    screen_home();
    ESP_LOGI(TAG, "home page up (strip renderer)");
}

void screen_home(void)
{
    s_word = "STARTING"; s_col = C_WHITE;
    paint(scene_page);
}

// The launcher: one tab's name (arrows + a dot pager) over up to four rows,
// the selected one highlighted. Left/Right page tabs; Up/Down move the row.
void screen_tabs(const char *tab, const char *const *items, int n, int sel, int tab_i, int tab_n)
{
    s_title = tab; s_items = items; s_n = n; s_sel = sel; s_tab = tab_i; s_ntab = tab_n;
    paint(scene_tabs);
}

void screen_status(const char *cue)
{
    if (!cue) return;
    if      (!strcmp(cue, "listen")) { s_word = "LISTEN";    s_col = C_GREEN; }
    else if (!strcmp(cue, "think"))  { s_word = "THINKING";  s_col = C_YELLOW; }
    else if (!strcmp(cue, "speak"))  { s_word = "SPEAKING";  s_col = C_CYAN; }
    else if (!strcmp(cue, "nolink")) { s_word = "NO LINK";   s_col = C_RED; }
    else if (!strcmp(cue, "idle"))   { s_word = "READY";     s_col = C_WHITE; }
    else return;                                       // unknown cue: leave the status as it is
    paint(scene_page);
}

// A border at the logical screen edges + distinct corner markers, so one look
// settles orientation (which corner is which) and offset (border flush?).
void screen_test(void)
{
    paint(scene_test);
}

// A measuring diagnostic: the whole 240x135 content filled gray, with an 8px
// coloured band on each edge (red=left, green=right, blue=top, yellow=bottom)
// and 8px dashes every 16px across the middle. On the glass: a full 8px band =
// that edge is flush; a thinner band = clipped by that much; black before a band
// = a margin there. The dashes let us count pixels of offset.
void screen_ruler(void)
{
    paint(scene_ruler);
}

void screen_cal(void)
{
    paint(scene_cal);
}

void screen_config(int fx, int fy, int gx, int gy)
{
    if (!panel || !paint_lock) return;
    if (xSemaphoreTake(paint_lock, pdMS_TO_TICKS(150)) != pdTRUE) return;
    flip_x = fx != 0;
    flip_y = fy != 0;
    gap_x = gx;
    gap_y = gy;
    clear_gram();                                      // wipe the whole panel: no ghost from the previous gap
    ESP_LOGI(TAG, "cfg flip=%d,%d gap=%d,%d", flip_x, flip_y, gap_x, gap_y);
    render_scene(scene_ruler);
    xSemaphoreGive(paint_lock);
}
