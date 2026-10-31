/* ALSA playback through the ioctls of the PCM device (no alsa-lib). */
#pragma once

#include "hal/hal.h"

struct pcm_alsa_params {
	const char *device;	/* "/dev/snd/pcmC0D0p" */
};

extern const struct hal_pcm_ops pcm_alsa;
