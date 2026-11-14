/*
 * Simulated mixer, Wi-Fi and battery for the PC platforms: the screens can
 * be tried and tested without the hardware. The Wi-Fi sees a few made-up
 * networks; any key of 8 or more characters starting with "pass" is right.
 * The battery is a directory named by $BARE_APP_SIM_BATTERY with the files
 * of a sysfs power_supply (capacity, status: "Discharging", "Charging"),
 * read as the board's are; none without it.
 */
#pragma once

#include "hal/hal.h"

extern const struct hal_mixer_ops mixer_sim;
extern const struct hal_wifi_ops wifi_sim;
extern const struct hal_power_ops power_sim;
