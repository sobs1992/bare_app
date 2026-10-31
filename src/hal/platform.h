/*
 * A platform is pure data: which drivers to use and with which parameters.
 * Adding a board with known hardware means adding one table in
 * src/platforms/ and listing it in platform.c.
 */
#pragma once

#include <stdint.h>

#include "core/input.h"
#include "hal.h"

#define PLATFORM_MAX_INPUTS 4

struct hal_dev {
	const void *ops;	/* struct hal_*_ops matching the field */
	const void *params;	/* driver specific */
};

struct platform {
	const char *name;
	/* Device tree "compatible" strings this platform takes; NULL-terminated */
	const char *const *compatible;
	struct hal_dev display;			/* hal_display_ops */
	struct hal_dev inputs[PLATFORM_MAX_INPUTS];	/* hal_input_ops */
	struct hal_dev power;			/* hal_power_ops, optional */
	struct hal_dev backlight;		/* hal_backlight_ops, optional */
	/* GP_BIT() mask: pressing all of them quits the application */
	uint32_t quit_combo;
};

/* By name if given, otherwise by /proc/device-tree/compatible, otherwise "pc" */
const struct platform *platform_probe(const char *name);
void platform_list(void);
/* Board model from the device tree, "" if none */
const char *platform_model(void);
