/*
 * Simulated mixer and Wi-Fi for the PC platforms: the screens can be
 * tried and tested without the hardware. The Wi-Fi sees a few made-up
 * networks; any key of 8 or more characters starting with "pass" is right.
 */
#pragma once

#include "hal/hal.h"

extern const struct hal_mixer_ops mixer_sim;
extern const struct hal_wifi_ops wifi_sim;
