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
#define PLATFORM_MAX_DISPLAYS 2

struct hal_dev {
	const void *ops;	/* struct hal_*_ops matching the field */
	const void *params;	/* driver specific */
};

struct platform {
	const char *name;
	/* Device tree "compatible" strings this platform takes; NULL-terminated */
	const char *const *compatible;
	/* hal_display_ops, tried in order until one opens */
	struct hal_dev displays[PLATFORM_MAX_DISPLAYS];
	struct hal_dev inputs[PLATFORM_MAX_INPUTS];	/* hal_input_ops */
	struct hal_dev power;			/* hal_power_ops, optional */
	struct hal_dev backlight;		/* hal_backlight_ops, optional */
	struct hal_dev mixer;			/* hal_mixer_ops, optional */
	struct hal_dev pcm;			/* hal_pcm_ops, optional: interface sounds */
	/* ALSA PCM device games play to (bare_app_doom -pcm), NULL: games are silent */
	const char *game_pcm;
	/*
	 * Heroes III (VCMI) at 1:1: its screen the frame's size, at least the
	 * 800x600 its windows need, the frame the part the cursor points at
	 * (BARE_APP_VCMI_NATIVE). false: drawn at twice the frame and taken
	 * 2:1 or as a 1:1 magnifier (L1).
	 */
	bool homm3_native;
	struct hal_dev wifi;			/* hal_wifi_ops, optional */
	/* GP_BIT() mask: pressing all of them quits the application */
	uint32_t quit_combo;
	/* Run after Quit in the menu has closed the application: turns the
	 * device off. NULL: Quit only quits (the quit combo always does). */
	const char *poweroff;
	/* Run for a short press of the power key: puts the device to sleep and
	 * returns once it has woken up. NULL: the key does nothing. */
	const char *suspend;
	/* settings file ("~/" = $HOME), NULL: nothing is kept between runs */
	const char *config_path;
};

/* By name if given, otherwise by /proc/device-tree/compatible, otherwise "pc" */
const struct platform *platform_probe(const char *name);
void platform_list(void);
/* Board model from the device tree, "" if none */
const char *platform_model(void);
