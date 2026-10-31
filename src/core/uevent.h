/*
 * Kernel uevents (NETLINK_KOBJECT_UEVENT) for one subsystem, delivered in
 * the loop of the thread that opened the monitor. Every monitor has its
 * own socket, so threads do not share anything.
 */
#pragma once

struct mainloop;
struct uevent_mon;

struct uevent {
	const char *action;	/* "add", "remove", "change"... */
	const char *subsystem;
	const char *devpath;
	const char *devname;	/* "input/event3", NULL if none */
	const char *name;	/* POWER_SUPPLY_NAME, NAME (input), NULL if none */
};

typedef void (*uevent_cb)(const struct uevent *ev, void *ud);

struct uevent_mon *uevent_open(struct mainloop *ml, const char *subsystem, uevent_cb cb, void *ud);
void uevent_close(struct uevent_mon *m);

/* Parse one netlink message: "action@devpath\0KEY=VALUE\0..." (for tests) */
int uevent_parse(char *buf, int len, struct uevent *ev);
