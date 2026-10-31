/*
 * One-shot notifications from control to view ("battery low", "charger
 * connected"). Unlike the context they are never merged: each one is
 * delivered.
 */
#pragma once

#include <stdbool.h>

enum notify_kind {
	NOTIFY_BATTERY_LOW,	/* value = capacity */
	NOTIFY_CHARGER,		/* value = 1 connected, 0 disconnected */
};

struct gui_notify {
	enum notify_kind kind;
	int value;
};

int notify_init(void);
void notify_deinit(void);

void gui_notify_post(enum notify_kind kind, int value);	/* any thread */
bool notify_pop(struct gui_notify *n);
int notify_fd(void);
void notify_ack(void);
