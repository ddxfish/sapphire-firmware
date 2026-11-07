// camera.c - see camera.h. esp32-camera on the LCD_CAM peripheral; SCCB on the
// camera's own two pins (a second I2C port); the frame lands in PSRAM by DMA as
// YUV 4:2:2 and esp_new_jpeg (the S3's SIMD encoder) makes the JPEG.
#include "camera.h"

#include <stdio.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio.h"
#include "board.h"
#include "light.h"
#include "power.h"

static const char *TAG = "camera";
#define XCLK_HZ 10000000               // Elecrow's own program runs the GC2145 at 10 MHz
#define XCLK_SLOW_HZ 5000000           // for a frame too big for the internal ring: the DMA writes PSRAM straight, and at 10 MHz it dropped bytes
#define JPEG_QUALITY 80                // esp_new_jpeg's scale: 1..100
#define SETTLE_COLD 6                  // frames before a shot on a sensor just started: its exposure finds the room (3 gave a dark shot, 2026-11-02)
#define SETTLE_WARM 1                  // ... on one that kept running
#define SETTLE_FLASH 3                 // ... with the light just turned on
#define CAMERA_WARM_S 300              // the sensor stays up this long after a shot: the next look is quick (7 KB internal in PSRAM mode: cheap)
#define WARN_MS 700
#define TRIES 3
#define RETRY_MS 100

static bool present, busy, warm;
static framesize_t warm_size;
static bool warm_psram;
static int warm_hz;
static esp_timer_handle_t warm_timer;
static char sensor_name[16];
static uint16_t pid;
static SemaphoreHandle_t lock;
static int encode_ms;
static enum { F_YUYV, F_UYVY, F_YUYV2, F_RGBBE, F_RGBLE } fmt = F_YUYV;
static const char *FMT_NAMES[] = {"yuyv", "uyvy", "yuyv2", "rgbbe", "rgble"};
bool camera_set_format(const char *name)
{
    for (int i = 0; i < 5; i++) if (!strcmp(name, FMT_NAMES[i])) { fmt = i; return true; }
    return false;
}
const char *camera_format(void) { return FMT_NAMES[fmt]; }
static bool dma_psram = true;        // measured 2026-11-02: the internal ring costs 35 KB internal RAM while the sensor is up, PSRAM 7 KB; 0.4 s slower warm
static int slow_hz = XCLK_SLOW_HZ;
void camera_set_xclk(int hz) { slow_hz = hz < 1000000 ? 1000000 : hz > 20000000 ? 20000000 : hz; }
bool camera_set_dma(const char *name) { if (!strcmp(name, "psram")) dma_psram = true; else if (!strcmp(name, "internal")) dma_psram = false; else return false; return true; }
const char *camera_dma(void) { return dma_psram ? "psram" : "internal"; }

static const struct { int w, h; framesize_t size; } SIZES[] = {
    {640, 480, FRAMESIZE_VGA}, {800, 600, FRAMESIZE_SVGA}, {1024, 768, FRAMESIZE_XGA},
    {1280, 1024, FRAMESIZE_SXGA}, {1600, 1200, FRAMESIZE_UXGA},
};
#define N_SIZES (int)(sizeof SIZES / sizeof SIZES[0])
#define DEFAULT_SIZE 0                 // 640x480: the internal ring at the full clock, under a second warm

static bool big(framesize_t size) { return size > FRAMESIZE_VGA || dma_psram; }   // more than the internal DMA ring takes (31 KB largest block, 2026-11-02), or asked

static camera_config_t config(framesize_t size)
{
    camera_config_t c = {
        .pin_pwdn = -1, .pin_reset = -1,
        .pin_xclk = BOARD_CAM_XCLK,
        .pin_sccb_sda = BOARD_CAM_SIOD, .pin_sccb_scl = BOARD_CAM_SIOC, .sccb_i2c_port = 1,
        .pin_d7 = BOARD_CAM_D7, .pin_d6 = BOARD_CAM_D6, .pin_d5 = BOARD_CAM_D5, .pin_d4 = BOARD_CAM_D4,
        .pin_d3 = BOARD_CAM_D3, .pin_d2 = BOARD_CAM_D2, .pin_d1 = BOARD_CAM_D1, .pin_d0 = BOARD_CAM_D0,
        .pin_vsync = BOARD_CAM_VSYNC, .pin_href = BOARD_CAM_HREF, .pin_pclk = BOARD_CAM_PCLK,
        .xclk_freq_hz = big(size) ? slow_hz : XCLK_HZ, .ledc_timer = LEDC_TIMER_1, .ledc_channel = LEDC_CHANNEL_1,
        .pixel_format = fmt >= F_RGBBE ? PIXFORMAT_RGB565 : PIXFORMAT_YUV422, .frame_size = size, .jpeg_quality = 12,
        .fb_count = 1, .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };
    return c;
}

static void down(void);
static void on_warm_end(void *arg)
{
    if (!lock || xSemaphoreTake(lock, 0) != pdTRUE) { esp_timer_start_once(warm_timer, 5 * 1000000); return; }   // a shot is on: later
    if (warm) { down(); ESP_LOGI(TAG, "the sensor is down again"); }
    xSemaphoreGive(lock);
}

static bool up(framesize_t size, char *why, size_t why_len)
{
    if (warm && warm_size == size && warm_psram == dma_psram && warm_hz == slow_hz) return true;
    if (warm) down();
    // NOT the PSRAM-direct DMA the speaker board uses: a raw 640x480 frame is 600 KB at the
    // sensor's pixel clock, and written straight into PSRAM the DMA dropped bytes and every
    // line slid sideways (2026-11-02). The driver's own ring in internal RAM copies out in time.
    esp_camera_set_psram_mode(big(size));   // small frames through the internal ring, big ones straight into PSRAM at the slow clock
    camera_config_t c = config(size);
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        if (why) snprintf(why, why_len, "the camera did not start (%s)", esp_err_to_name(err));
        return false;
    }
    warm = true;
    warm_size = size;
    warm_psram = dma_psram;
    warm_hz = slow_hz;
    return true;
}

static void down(void)
{
    if (!warm) return;
    esp_camera_deinit();
    warm = false;
}

void camera_sleep(void)
{
    if (!lock) return;
    if (warm_timer) esp_timer_stop(warm_timer);
    xSemaphoreTake(lock, portMAX_DELAY);
    down();
    xSemaphoreGive(lock);
}

bool camera_has_flash(void) { return true; }

void camera_start(void)
{
    lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t t = {.callback = on_warm_end, .name = "camera-warm"};
    esp_timer_create(&t, &warm_timer);
    char why[80] = {0};
    if (!up(FRAMESIZE_VGA, why, sizeof why)) { ESP_LOGI(TAG, "no camera answered (%s)", why); return; }
    sensor_t *s = esp_camera_sensor_get();
    camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
    pid = s ? s->id.PID : 0;
    strlcpy(sensor_name, info && info->name ? info->name : "camera", sizeof sensor_name);
    present = true;
    down();
    ESP_LOGI(TAG, "%s on the plug (pid 0x%x): the driver sleeps until a shot", sensor_name, pid);
}

bool camera_present(void) { return present; }
const char *camera_sensor_name(void) { return sensor_name; }
bool camera_busy(void) { return busy || warm; }
int camera_last_encode_ms(void) { return encode_ms; }

cJSON *camera_sizes(void)
{
    cJSON *a = cJSON_CreateArray();
    char text[16];
    for (int i = 0; present && i < N_SIZES; i++) {
        snprintf(text, sizeof text, "%dx%d", SIZES[i].w, SIZES[i].h);
        cJSON_AddItemToArray(a, cJSON_CreateString(text));
    }
    return a;
}

// YUV 4:2:2 as the sensor gives it (Y Cb Y Cr) to a JPEG, 4:2:0 inside.
static bool encode(const uint8_t *yuv, int w, int h, snap_t *out, char *why, size_t why_len)
{
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = w;
    cfg.height = h;
    cfg.src_type = fmt == F_UYVY ? JPEG_PIXEL_FORMAT_CbYCrY : fmt == F_RGBBE ? JPEG_PIXEL_FORMAT_RGB565_BE : fmt == F_RGBLE ? JPEG_PIXEL_FORMAT_RGB565_LE : JPEG_PIXEL_FORMAT_YCbYCr;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = JPEG_QUALITY;
    cfg.rotate = JPEG_ROTATE_0D;
    cfg.task_enable = false;
    jpeg_enc_handle_t enc = NULL;
    if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK || !enc) { strlcpy(why, "the JPEG encoder did not open", why_len); return false; }
    size_t room = (size_t)w * h / 4;                        // 2 bits a pixel: quality 80 at 4:2:0 runs 1 to 1.5 (a 1600x1200 frame is 3.8 MB of the PSRAM already)
    uint8_t *jpeg = heap_caps_aligned_alloc(16, room, MALLOC_CAP_SPIRAM);
    if (!jpeg) { jpeg_enc_close(enc); strlcpy(why, "no memory for the picture", why_len); return false; }
    int64_t t0 = esp_timer_get_time();
    int len = 0;
    jpeg_error_t err = jpeg_enc_process(enc, yuv, w * h * 2, jpeg, room, &len);
    encode_ms = (int)((esp_timer_get_time() - t0) / 1000);
    jpeg_enc_close(enc);
    if (err != JPEG_ERR_OK || len <= 0) { free(jpeg); snprintf(why, why_len, "the JPEG encoder failed (%d)", (int)err); return false; }
    out->jpeg = jpeg;
    out->len = len;
    out->w = w;
    out->h = h;
    return true;
}

static bool shoot(int pick, bool flash, snap_t *out, char *why, size_t why_len)
{
    bool cold = !(warm && warm_size == SIZES[pick].size && warm_psram == dma_psram && warm_hz == slow_hz);
    if (!up(SIZES[pick].size, why, why_len)) return false;
    if (flash) board_fill_light(true);
    bool ok = false;
    camera_fb_t *fb = NULL;
    int settle = cold ? SETTLE_COLD : flash ? SETTLE_FLASH : SETTLE_WARM;
    for (int n = 0; n < settle; n++) {
        fb = esp_camera_fb_get();
        if (!fb) break;
        esp_camera_fb_return(fb);
    }
    fb = fb ? esp_camera_fb_get() : NULL;
    if (fb && fmt == F_YUYV2) for (size_t i = 0; i + 1 < fb->len; i += 2) { uint8_t t = fb->buf[i]; fb->buf[i] = fb->buf[i + 1]; fb->buf[i + 1] = t; }
    if (fb && fb->len >= (size_t)fb->width * fb->height * 2) ok = encode(fb->buf, fb->width, fb->height, out, why, why_len);
    else if (fb) snprintf(why, why_len, "the camera gave a short frame (%u bytes)", (unsigned)fb->len);
    else strlcpy(why, "the camera gave no frame", why_len);
    if (fb) esp_camera_fb_return(fb);
    if (flash) board_fill_light(false);
    esp_timer_stop(warm_timer);
    esp_timer_start_once(warm_timer, CAMERA_WARM_S * 1000000LL);   // down again after a while with no shot
    return ok;
}

bool camera_snap(int w, int h, bool flash, snap_t *out, char *why, size_t why_len)
{
    memset(out, 0, sizeof *out);
    if (!present) { strlcpy(why, "this board has no camera", why_len); return false; }
    if (!lock || xSemaphoreTake(lock, 0) != pdTRUE) { strlcpy(why, "a picture is already being taken", why_len); return false; }
    busy = true;
    int pick = DEFAULT_SIZE;
    for (int i = 0; i < N_SIZES; i++) if (SIZES[i].w == w && SIZES[i].h == h) pick = i;
    light_cue("camera");
    audio_tone(880, 120);
    vTaskDelay(pdMS_TO_TICKS(60));
    audio_tone(1320, 160);
    vTaskDelay(pdMS_TO_TICKS(WARN_MS));
    bool ok = false;
    for (int n = 0; n < TRIES && !ok; n++) {
        if (n) {
            ESP_LOGW(TAG, "try %d of %d: %s", n + 1, TRIES, why);
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
        }
        ok = shoot(pick, flash, out, why, why_len);
    }
    light_camera_done();
    busy = false;
    xSemaphoreGive(lock);
    if (ok) ESP_LOGI(TAG, "snap %dx%d as %s%s, %u bytes, encoded in %d ms", out->w, out->h, FMT_NAMES[fmt], flash ? " with the light" : "", (unsigned)out->len, encode_ms);
    else ESP_LOGW(TAG, "no snap: %s", why);
    return ok;
}

void camera_snap_done(snap_t *s)
{
    free(s->jpeg);
    s->jpeg = NULL;
    s->len = 0;
}
