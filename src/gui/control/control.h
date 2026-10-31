/*
 * control: the system side of the GUI (control thread). Modules are cut by
 * data domain (power, display, navigation...), not by widget: each one
 * listens to the system and writes its own context section. No toolkit
 * headers here.
 */
#pragma once

#include "gui/action.h"

struct mainloop;
struct platform;

struct control_env {
	struct mainloop *ml;
	const struct platform *pf;
	const char *lang;	/* -L / BARE_APP_LANG, overrides the saved one */
};

struct control_ops {
	const char *name;
	int (*init)(const struct control_env *env);
	void (*on_action)(const struct gui_action *a);	/* may be NULL */
	void (*deinit)(void);				/* may be NULL */
};

int control_start(const struct control_env *env);
void control_stop(void);
/* The power-off command to run once everything has stopped, NULL: just exit */
const char *control_poweroff_cmd(void);

extern const struct control_ops ctl_app, ctl_nav, ctl_power, ctl_display, ctl_system, ctl_settings,
	ctl_audio, ctl_wifi;
