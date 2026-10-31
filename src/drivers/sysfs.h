/* Power supply and backlight through sysfs class devices. */
#pragma once

#include "hal/hal.h"

struct power_sysfs_params {
	const char *battery;	/* "/sys/class/power_supply/rk817-battery" */
	const char *charger;	/* may be NULL */
};

struct backlight_sysfs_params {
	const char *path;	/* "/sys/class/backlight/backlight" */
	int min_percent;	/* never go darker than this */
};

extern const struct hal_power_ops power_sysfs;
extern const struct hal_backlight_ops backlight_sysfs;
