#pragma once

#include "hal/hal.h"

/*
 * Display in memory, for tests without a screen. If the environment
 * variable BARE_APP_SHOT names a file, the last frame is written there as
 * a PPM image on close.
 */
struct mem_display_params {
	int width, height;
};

extern const struct hal_display_ops display_mem;
