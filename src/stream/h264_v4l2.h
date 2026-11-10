/*
 * H.264 encoder of the stream in hardware: a V4L2 mem2mem encoder that
 * takes XRGB frames (V4L2 XBGR32, as DRM XRGB8888) and gives H.264, such
 * as meson-venc on the GO Ultra (Amlogic HCODEC). Same contract as h264.h.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hal/hal.h"

struct h264_v4l2;

/* NULL when the system has no such encoder */
struct h264_v4l2 *h264_v4l2_new(int width, int height, int fps, int kbps);
void h264_v4l2_free(struct h264_v4l2 *e);
int h264_v4l2_params(struct h264_v4l2 *e, const uint8_t **data, size_t *size);
void h264_v4l2_force_idr(struct h264_v4l2 *e);
int h264_v4l2_encode(struct h264_v4l2 *e, const struct hal_fb *frame, const uint8_t **data,
		     size_t *size, int *idr);
/* the device, for the log */
const char *h264_v4l2_name(const struct h264_v4l2 *e);
