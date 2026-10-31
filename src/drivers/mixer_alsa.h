/* ALSA mixer through the ioctls of the control device (no alsa-lib). */
#pragma once

#include "hal/hal.h"

struct mixer_alsa_params {
	const char *device;		/* "/dev/snd/controlC0" */
	const char *volume;		/* integer control; every channel gets the same value */
	const char *route;		/* enumerated control choosing the output, NULL: none */
	const char *route_speaker;	/* its items */
	const char *route_headphones;
	const char *jack;		/* boolean jack control, NULL: none */
};

extern const struct hal_mixer_ops mixer_alsa;
