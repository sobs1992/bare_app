/*
 * H.264 encoder of the stream: XRGB frames in, the NAL units of each frame
 * out (Annex B, start codes kept). A V4L2 hardware encoder when the system
 * has one (h264_v4l2.c; meson-venc on the GO Ultra), else OpenH264 (BSD
 * licence). BARE_APP_STREAM_SW=1 takes OpenH264 anyway.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hal/hal.h"

struct h264;

/* width, height even; kbps the bit rate aimed at */
struct h264 *h264_new(int width, int height, int fps, int kbps);
void h264_free(struct h264 *e);
/* The SPS and PPS as Annex B; 0 or -1 */
int h264_params(struct h264 *e, const uint8_t **data, size_t *size);
void h264_force_idr(struct h264 *e);
/* The bit rate aimed at from the next frame on */
void h264_set_bitrate(struct h264 *e, int kbps);
/*
 * One frame: data and size of the access unit (Annex B), valid until the next
 * call; size 0 when the encoder skipped the frame. -1 on an error.
 */
int h264_encode(struct h264 *e, const struct hal_fb *frame, uint64_t ms, const uint8_t **data,
		size_t *size, int *idr);
