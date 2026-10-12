// camera.c - see camera.h. esp32-camera on the LCD_CAM peripheral; the sensor's
// control channel (SCCB) rides our I2C bus (sccb_i2c_port 0, the new driver),
// the frame lands in PSRAM by DMA, the sensor encodes the JPEG itself.
#include "camera.h"

#include <stdio.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio.h"
#include "board.h"
#include "light.h"

static const char *TAG = "camera";
#define XCLK_HZ 20000000
#define JPEG_QUALITY 12                // 0 best .. 63; 12 = good and a 800x600 frame of 40-70 KB
#define SETTLE_FRAMES 3                // the exposure finds the room over a few frames
#define WARN_MS 700                    // the ring blinks and two notes sound before the shot

static bool present, busy;
static char sensor_name[16];
static uint16_t pid;
static SemaphoreHandle_t lock;

static const struct { int w, h; framesize_t size; } SIZES[] = {
    {640, 480, FRAMESIZE_VGA}, {800, 600, FRAMESIZE_SVGA}, {1024, 768, FRAMESIZE_XGA},
    {1280, 1024, FRAMESIZE_SXGA}, {1600, 1200, FRAMESIZE_UXGA}, {2560, 1920, FRAMESIZE_QSXGA},
};
#define DEFAULT_SIZE 1                 // 800x600

static int largest(void) { return pid == OV5640_PID ? 5 : 4; }   // an OV2640 or OV3660 tops out at 1600x1200 here

static camera_config_t config(framesize_t size)
{
    camera_config_t c = {
        .pin_pwdn = -1, .pin_reset = -1,                          // both are on the expander (board.c)
        .pin_xclk = BOARD_CAM_XCLK,
        .pin_sccb_sda = -1, .pin_sccb_scl = -1, .sccb_i2c_port = 0,   // the shared bus, already up
        .pin_d7 = BOARD_CAM_D7, .pin_d6 = BOARD_CAM_D6, .pin_d5 = BOARD_CAM_D5, .pin_d4 = BOARD_CAM_D4,
        .pin_d3 = BOARD_CAM_D3, .pin_d2 = BOARD_CAM_D2, .pin_d1 = BOARD_CAM_D1, .pin_d0 = BOARD_CAM_D0,
        .pin_vsync = BOARD_CAM_VSYNC, .pin_href = BOARD_CAM_HREF, .pin_pclk = BOARD_CAM_PCLK,
        .xclk_freq_hz = XCLK_HZ, .ledc_timer = LEDC_TIMER_1, .ledc_channel = LEDC_CHANNEL_1,   // timer 0 is the backlight
        .pixel_format = PIXFORMAT_JPEG, .frame_size = size, .jpeg_quality = JPEG_QUALITY,
        .fb_count = 1, .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };
    return c;
}

static bool up(framesize_t size, char *why, size_t why_len)
{
    board_camera_power(true);
    vTaskDelay(pdMS_TO_TICKS(30));
    // Frames go straight into the PSRAM buffer by DMA. The driver's other way
    // wants 16 KB of internal RAM in one piece at every shot, and a board that
    // has run for a while has 13 (2026-10-08: "the camera did not start").
    esp_camera_set_psram_mode(true);
    camera_config_t c = config(size);
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        board_camera_power(false);
        if (why) snprintf(why, why_len, "the camera did not start (%s)", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void down(void)
{
    esp_camera_deinit();
    board_camera_power(false);
}

void camera_start(void)
{
    lock = xSemaphoreCreateMutex();
    char why[80] = {0};
    if (!up(FRAMESIZE_VGA, why, sizeof why)) { ESP_LOGI(TAG, "no camera on the plug (%s)", why); return; }
    sensor_t *s = esp_camera_sensor_get();
    camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
    pid = s ? s->id.PID : 0;
    strlcpy(sensor_name, info && info->name ? info->name : "camera", sizeof sensor_name);
    present = true;
    down();
    ESP_LOGI(TAG, "%s on the plug (pid 0x%x): powered down until a shot", sensor_name, pid);
}

bool camera_present(void) { return present; }
const char *camera_sensor_name(void) { return sensor_name; }
bool camera_busy(void) { return busy; }

cJSON *camera_sizes(void)
{
    cJSON *a = cJSON_CreateArray();
    char text[16];
    for (int i = 0; present && i <= largest(); i++) {
        snprintf(text, sizeof text, "%dx%d", SIZES[i].w, SIZES[i].h);
        cJSON_AddItemToArray(a, cJSON_CreateString(text));
    }
    return a;
}

bool camera_snap(int w, int h, snap_t *out, char *why, size_t why_len)
{
    memset(out, 0, sizeof *out);
    if (!present) { strlcpy(why, "this board has no camera", why_len); return false; }
    if (!lock || xSemaphoreTake(lock, 0) != pdTRUE) { strlcpy(why, "a picture is already being taken", why_len); return false; }
    busy = true;
    int pick = DEFAULT_SIZE;
    for (int i = 0; i <= largest(); i++) if (SIZES[i].w == w && SIZES[i].h == h) pick = i;
    // the room is warned first, as the Pi does: white blinks and two notes
    light_cue("camera");
    audio_tone(880, 120);
    vTaskDelay(pdMS_TO_TICKS(60));
    audio_tone(1320, 160);
    vTaskDelay(pdMS_TO_TICKS(WARN_MS));
    bool ok = false;
    if (up(SIZES[pick].size, why, why_len)) {
        camera_fb_t *fb;
        for (int n = 0; n < SETTLE_FRAMES; n++) {
            fb = esp_camera_fb_get();
            if (fb) esp_camera_fb_return(fb);
        }
        fb = esp_camera_fb_get();
        if (fb && fb->len > 4) {
            out->jpeg = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
            if (out->jpeg) {
                memcpy(out->jpeg, fb->buf, fb->len);
                out->len = fb->len; out->w = fb->width; out->h = fb->height;
                ok = true;
            } else strlcpy(why, "no memory for the picture", why_len);
        } else strlcpy(why, "the camera gave no frame", why_len);
        if (fb) esp_camera_fb_return(fb);
        down();
    }
    light_camera_done();                            // back to the brain's cue, or idle
    busy = false;
    xSemaphoreGive(lock);
    if (ok) ESP_LOGI(TAG, "snap %dx%d, %u bytes", out->w, out->h, (unsigned)out->len);
    else ESP_LOGW(TAG, "no snap: %s", why);
    return ok;
}

void camera_snap_done(snap_t *s)
{
    free(s->jpeg);
    s->jpeg = NULL;
    s->len = 0;
}
