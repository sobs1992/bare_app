#pragma once

#include <stdbool.h>

#include "hal/hal.h"

/*
 * Display in memory, for tests without a screen (snapshots: see
 * BARE_APP_SHOT in the view core).
 *
 * With video_layer it behaves like the DRM display with planes: a video
 * layer under a UI layer with alpha.
 */
struct mem_display_params {
	int width, height;
	bool video_layer;
};

extern const struct hal_display_ops display_mem;
