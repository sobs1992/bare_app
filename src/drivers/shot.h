/* Screenshot of the composed layers (logical orientation) as a PPM image. */
#pragma once

#include "hal/hal.h"

/* video may be NULL; ui may have straight alpha (HAL_FMT_ARGB8888) */
int shot_write_ppm(const char *path, const struct hal_fb *ui, const struct hal_fb *video);
