// camera.h - the eye: the GC2145 (or an OV2640) on the board's DVP plug. It idles
// powered (no power-down pin reaches the chip; only the whole rail cuts it): the
// driver is started for a shot and stopped after, which is what costs the RAM.
// A snap: warn the room (the LED blinks, two notes), start the sensor, let the
// exposure settle over a few frames, take one YUV frame into PSRAM, encode it to
// JPEG on the S3 (the GC2145 makes none of its own), stop the sensor.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct { uint8_t *jpeg; size_t len; int w, h; } snap_t;   // jpeg is in PSRAM until camera_snap_done

void camera_start(void);               // the probe at boot
bool camera_present(void);
const char *camera_sensor_name(void);  // "GC2145"; "" when none answered
cJSON *camera_sizes(void);             // ["640x480", ...], what this sensor does, for /health
bool camera_busy(void);
// Blocks a second or a few. w x h from camera_sizes, or 640x480 when it is not one of them. `flash`: the
// white LED beside the lens is on for the shot (board_fill_light). After a shot the sensor stays up
// for CAMERA_WARM_S so the next one is quick; a sleep puts it down (camera_sleep).
bool camera_snap(int w, int h, bool flash, snap_t *out, char *why, size_t why_len);
bool camera_has_flash(void);           // this board has a light beside the lens: /health camera.flash
void camera_sleep(void);               // the sensor down now (before the rail goes off)
void camera_snap_done(snap_t *s);
int camera_last_encode_ms(void);       // how long the last JPEG took to make
// The bench knob (2026-11-02): what the sensor is asked for and how the encoder reads it.
// "yuyv" (YUV422, Y Cb Y Cr), "uyvy" (YUV422 read as Cb Y Cr Y), "yuyv2" (YUV422 with each 16-bit
// word swapped first), "rgbbe" / "rgble" (RGB565, big or little endian). False for another word.
bool camera_set_format(const char *name);
const char *camera_format(void);
// "internal" (the driver's ring in internal RAM, 36 KB while the sensor is up, the full clock) or "psram"
// (DMA straight into PSRAM at the slow clock, nothing internal to speak of): the bench knob for small frames
bool camera_set_dma(const char *name);
const char *camera_dma(void);
void camera_set_xclk(int hz);          // the sensor clock in PSRAM mode (5 MHz proven whole at 800x600; 10 tore): the bench knob
