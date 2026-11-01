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
};

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
void gui_action_post_secret(enum gui_action_type type, const char *str, const char *secret);
bool action_pop(struct gui_action *a);
int action_fd(void);
void action_ack(void);
const char *action_name(enum gui_action_type type);
