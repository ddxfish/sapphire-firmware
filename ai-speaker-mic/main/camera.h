// camera.h - the eye: a DVP camera on the 24 pin plug (the kit's OV5640; any
// OV2640/OV3660/OV5640 is recognised). Powered ONLY for a shot: EXIO5 holds
// it down between them (ruled 2026-10-07: power, and the sensor is
// physically off). At boot it is woken once to say who it is, then put down.
// A snap: warn the room (white blinks, two notes), power up, let the exposure
// settle over a few frames, take one JPEG straight from the sensor, power down.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct { uint8_t *jpeg; size_t len; int w, h; } snap_t;   // jpeg is in PSRAM until camera_snap_done

void camera_start(void);               // the probe at boot
bool camera_present(void);
const char *camera_sensor_name(void);       // "OV5640"; "" when none answered
cJSON *camera_sizes(void);             // ["640x480", ...], what this sensor does, for /health
bool camera_busy(void);
// Blocks a few seconds. w x h from camera_sizes, or 800x600 when it is not one of them.
bool camera_snap(int w, int h, snap_t *out, char *why, size_t why_len);
void camera_snap_done(snap_t *s);
