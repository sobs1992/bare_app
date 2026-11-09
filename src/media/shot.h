/* The composed layers (logical orientation): as a PPM image, into a frame. */
#pragma once

#include "hal/hal.h"

/* video may be NULL; ui may have straight alpha (HAL_FMT_ARGB8888) */
int shot_write_ppm(const char *path, const struct hal_fb *ui, const struct hal_fb *video);
/* The same into dst (XRGB8888, at least ui's size) */
void shot_compose(struct hal_fb *dst, const struct hal_fb *ui, const struct hal_fb *video);
