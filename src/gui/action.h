/*
 * Actions: the only way the view asks control to do something.
 * Posted from the view thread, executed in the control thread.
 */
#pragma once

#include <stdbool.h>

enum gui_action_type {
	ACT_BACK,		/* close the current screen */
	ACT_OPEN_SCREEN,	/* arg = enum screen_id */
	ACT_SET_BRIGHTNESS,	/* arg = percent */
	ACT_SET_LANG,		/* str = language code */
	ACT_SET_VOLUME,		/* arg = percent */
	ACT_VOLUME_STEP,	/* arg = +/- percent (volume keys: shows the level) */
	ACT_SET_AUDIO_OUT,	/* arg = enum audio_output */
	ACT_WIFI_ENABLE,	/* arg = 1 on, 0 off */
	ACT_WIFI_SCAN,
	ACT_WIFI_ASK_KEY,	/* str = network the key screen is for */
	ACT_WIFI_CONNECT,	/* str = network, secret = key ("" none: open or saved) */
	ACT_WIFI_FORGET,	/* str = network */
	ACT_QUIT,
	ACT_POWER_OFF,		/* Quit in the menu, holding the power key: quits and turns
				   the device off if the platform can */
	ACT_SCREEN_OFF,		/* arg = 1 backlight off (sleep, first stage), 0 back on */
	ACT_SUSPEND,		/* sleep until woken (control answers NOTIFY_RESUMED) */
	ACT_USB_DISK,		/* arg = 1 share the games partition again, 0 take it back */
	ACT_SYSMON,		/* arg = enum sysmon_mode (ctx.h) */
	ACT_GAME_PLAYED,	/* str = game_key() of the game that has just started */
	ACT_SET_GAME_HINT,	/* arg = 1 show the controls when a game starts, 0 not */
	ACT_SET_CONTROL,	/* str = game_key(), arg = ACT_CONTROL_ARG(button, for the game) */
	ACT_RESET_CONTROLS,	/* str = game_key(): its controls back to the default */
	ACT_SET_INVERT,		/* str = game_key(), arg = ACT_INVERT_ARG(axis, inverted) */
	ACT_SET_GYRO,		/* str = game_key(), arg = ACT_GYRO_ARG(on, sensitivity) */
	ACT_SET_STREAM,		/* arg = 1 stream the screen and the sound, 0 not */
	ACT_SET_HIBERNATE,	/* arg = 1 sleep by hibernating (platform hibernate), 0 not */
	ACT_SET_CLOCK,		/* str = "YYYY-MM-DD hh:mm": the date and time to set */
	ACT_INPUT_DEVICES,	/* arg = INPUT_DEV_* plugged in now (the view's drivers saw it) */
};

/* ACT_SET_CONTROL: button of the device, enum gp_button it is for the game (GP_NONE: none) */
#define ACT_CONTROL_ARG(from, to) ((from) | ((to) + 1) << 8)
#define ACT_CONTROL_FROM(arg) ((arg) & 0xff)
#define ACT_CONTROL_TO(arg) (((arg) >> 8) - 1)
/* ACT_SET_INVERT: enum gp_axis, the game gets it the other way round or not */
#define ACT_INVERT_ARG(axis, on) ((axis) | !!(on) << 8)
#define ACT_INVERT_AXIS(arg) ((arg) & 0xff)
#define ACT_INVERT_ON(arg) (((arg) >> 8) & 1)

/* ACT_SET_GYRO: GP_GYRO_GAME, 0 or 1; the sensitivity in tenths (0: as it was) */
#define ACT_GYRO_ARG(on, sens) (((on) + 2) | (sens) << 8)
#define ACT_GYRO_ON(arg) (((arg) & 0xff) - 2)
#define ACT_GYRO_SENS(arg) (((arg) >> 8) & 0xff)

struct gui_action {
	enum gui_action_type type;
	int arg;
	char str[33];		/* a language code, a network name */
	char secret[64];	/* a Wi-Fi key: never logged */
};

int action_init(void);
void action_deinit(void);

void gui_action_post(enum gui_action_type type, int arg);	/* any thread */
void gui_action_post_str(enum gui_action_type type, const char *str);
void gui_action_post_str_arg(enum gui_action_type type, const char *str, int arg);
void gui_action_post_secret(enum gui_action_type type, const char *str, const char *secret);
bool action_pop(struct gui_action *a);
int action_fd(void);
void action_ack(void);
const char *action_name(enum gui_action_type type);
