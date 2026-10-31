/*
 * ODROID-GO Advance (rev 1.0, rev 1.1 Black Edition) and ODROID-GO Super.
 * Codes come from rk3326-odroid-go*.dts; a revision without some key or
 * axis simply never reports it.
 */
#include <linux/input-event-codes.h>
#include <stddef.h>

#include "drivers/display_drm.h"
#include "drivers/display_fbdev.h"
#include "drivers/input_evdev.h"
#include "drivers/sysfs.h"
#include "hal/platform.h"

static const struct evdev_key_map oga_keys[] = {
	{ BTN_DPAD_UP, GP_UP }, { BTN_DPAD_DOWN, GP_DOWN },
	{ BTN_DPAD_LEFT, GP_LEFT }, { BTN_DPAD_RIGHT, GP_RIGHT },
	{ BTN_EAST, GP_A }, { BTN_SOUTH, GP_B }, { BTN_NORTH, GP_X }, { BTN_WEST, GP_Y },
	{ BTN_TL, GP_L1 }, { BTN_TR, GP_R1 }, { BTN_TL2, GP_L2 }, { BTN_TR2, GP_R2 },
	{ BTN_SELECT, GP_SELECT }, { BTN_START, GP_START },
	{ BTN_TRIGGER_HAPPY1, GP_F1 }, { BTN_TRIGGER_HAPPY2, GP_F2 },
	{ BTN_TRIGGER_HAPPY3, GP_F3 }, { BTN_TRIGGER_HAPPY4, GP_F4 },
	{ BTN_TRIGGER_HAPPY5, GP_F5 }, { BTN_TRIGGER_HAPPY6, GP_F6 },
	{ KEY_VOLUMEUP, GP_VOL_UP }, { KEY_VOLUMEDOWN, GP_VOL_DOWN },
	{ 0 }
};

static const struct evdev_axis_map oga_axes[] = {
	{ ABS_X, GP_AXIS_LX, false }, { ABS_Y, GP_AXIS_LY, false },
	{ ABS_RX, GP_AXIS_RX, false }, { ABS_RY, GP_AXIS_RY, false },	/* Super only */
	{ 0, GP_AXIS_COUNT, false }
};

static const char *const oga_compatible[] = {
	"hardkernel,rk3326-odroid-go2",
	"hardkernel,rk3326-odroid-go2-v11",
	"hardkernel,rk3326-odroid-go3",
	NULL
};

const struct platform platform_oga = {
	.name = "oga",
	.compatible = oga_compatible,
	.displays = {
		/* planes: video under the UI; fbdev if DRM is busy or missing */
		{ &display_drm, &(const struct drm_params){
			.device = "/dev/dri/card0", .tty = "/dev/tty1", .rotation = HAL_ROT_AUTO } },
		{ &display_fbdev, &(const struct fbdev_params){
			.device = "/dev/fb0", .tty = "/dev/tty1", .rotation = HAL_ROT_AUTO } },
	},
	.inputs = {
		{ &input_evdev, &(const struct evdev_params){ .name = "gpio-keys", .keys = oga_keys } },
		{ &input_evdev, &(const struct evdev_params){
			.name = "adc-joystick", .axes = oga_axes, .deadzone = 0.15f } },
	},
	.power = { &power_sysfs, &(const struct power_sysfs_params){
		.battery = "/sys/class/power_supply/rk817-battery",
		.charger = "/sys/class/power_supply/rk817-charger" } },
	.backlight = { &backlight_sysfs, &(const struct backlight_sysfs_params){
		.path = "/sys/class/backlight/backlight", .min_percent = 5 } },
	.quit_combo = GP_BIT(GP_F6),
};
