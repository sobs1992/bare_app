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
	NOTIFY_SNAPSHOT,	/* debug: write a screenshot (handled by the view core) */
	NOTIFY_VOLUME,		/* value = percent, after a volume key */
	NOTIFY_WIFI_CONNECTED,	/* to the network the user picked */
	NOTIFY_WIFI_WRONG_KEY,
	NOTIFY_WIFI_FAILED,	/* the connection could not be started */
	NOTIFY_RESUMED,		/* after ACT_SUSPEND: value 0 woken up, -1 did not sleep */
	NOTIFY_QUIT_HOLD,	/* view core to backend: quit combo held, value = ms
				   until it quits; 0 released before */
	NOTIFY_POWER_HOLD,	/* the same for the power key and turning the device off */
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
