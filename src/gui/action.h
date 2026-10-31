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
	ACT_QUIT,
};

struct gui_action {
	enum gui_action_type type;
	int arg;
	char str[16];
};

int action_init(void);
void action_deinit(void);

void gui_action_post(enum gui_action_type type, int arg);	/* any thread */
void gui_action_post_str(enum gui_action_type type, const char *str);
bool action_pop(struct gui_action *a);
int action_fd(void);
void action_ack(void);
const char *action_name(enum gui_action_type type);
