#pragma once

#include "hal/hal.h"

struct fbdev_params {
	const char *device;	/* "/dev/fb0" */
	const char *tty;	/* switched to KD_GRAPHICS while running, may be NULL */
	int rotation;		/* enum hal_rotation or HAL_ROT_AUTO */
};

extern const struct hal_display_ops display_fbdev;
