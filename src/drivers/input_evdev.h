#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/input.h"
#include "hal/hal.h"

/* evdev key code -> logical button; the table ends with code 0 */
struct evdev_key_map {
	uint16_t code;
	enum gp_button button;
};

/* evdev ABS_* code -> logical axis; ends with axis GP_AXIS_COUNT */
struct evdev_axis_map {
	uint16_t code;
	enum gp_axis axis;
	bool invert;
};

struct evdev_params {
	const char *name;	/* device name (EVIOCGNAME), e.g. "gpio-keys" */
	const struct evdev_key_map *keys;	/* may be NULL */
	const struct evdev_axis_map *axes;	/* may be NULL */
	float deadzone;		/* for axes, 0..1 */
};

extern const struct hal_input_ops input_evdev;
