#pragma once

#include "hal/hal.h"

/*
 * Scripted input from stdin (only when stdin is not a terminal): words
 * separated by spaces or newlines. A button name (see gp_button_name())
 * is a press and release, "NAME:MS" holds it MS ms, "wait:MS" pauses. One
 * word per 'interval_ms'.
 *
 *   echo "down a wait:500 b f6:2500" | bare_app --platform pc
 *
 * BARE_APP_SIM_DEVICES=<INPUT_DEV_* mask> says those input devices are
 * plugged in (the status bar's icons in screenshots: 7 all of them).
 */
struct script_params {
	int interval_ms;
};

extern const struct hal_input_ops input_script;
