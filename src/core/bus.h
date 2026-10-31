/*
 * System event bus (control thread). Services post events from any
 * thread; subscribers run in the control loop, in posting order, never
 * nested inside another delivery.
 */
#pragma once

#include "hal/hal.h"

struct mainloop;

enum ev_type {
	EV_POWER_CHANGED,	/* u.power */
	EV_MIXER_CHANGED,	/* u.mixer */
	EV_WIFI_STATUS,		/* u.wifi */
	EV_WIFI_SCAN,		/* new results for wifi_networks() */
	EV_WIFI_WRONG_KEY,	/* a network refused the key */
	EV_COUNT
};

struct ev {
	enum ev_type type;
	union {
		struct hal_power_state power;
		struct hal_mixer_state mixer;
		struct {
			bool available;		/* the daemon answers */
			bool enabled;		/* wifi_set_enabled() */
			struct hal_wifi_status st;	/* link HAL_WIFI_DOWN when off */
		} wifi;
	} u;
};

typedef void (*bus_cb)(const struct ev *e, void *ud);

int bus_init(struct mainloop *ml);
void bus_deinit(void);

void bus_post(const struct ev *e);			/* any thread */
int bus_subscribe(enum ev_type type, bus_cb cb, void *ud);	/* id > 0 */
void bus_unsubscribe(int id);
