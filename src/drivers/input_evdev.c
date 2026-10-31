/*
 * evdev input: finds /dev/input/eventN by device name (the number depends
 * on driver probe order) and maps its keys and axes to the logical gamepad.
 * Axes are normalised with the range from EVIOCGABS; the value at open
 * time is taken as the stick center.
 */
#include "input_evdev.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/joystick.h"
#include "core/log.h"
#include "core/mainloop.h"

#define TAG "evdev"
#define MAX_AXES 8

struct evdev {
	struct mainloop *ml;
	const struct evdev_params *p;
	int fd;
	char path[32];
	struct {
		uint16_t code;
		enum gp_axis axis;
		struct joy_axis_cal cal;
	} axes[MAX_AXES];
	int naxes;
};

static int open_by_name(const char *want, char *path, size_t size)
{
	char name[128];
	int i, fd;

	for (i = 0; i < 64; i++) {
		snprintf(path, size, "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(name, 0, sizeof(name));
		if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && !strcmp(name, want))
			return fd;
		close(fd);
	}
	return -1;
}

static void setup_axes(struct evdev *ev)
{
	const struct evdev_axis_map *m;

	for (m = ev->p->axes; m && m->axis != GP_AXIS_COUNT && ev->naxes < MAX_AXES; m++) {
		struct input_absinfo ai;

		if (ioctl(ev->fd, EVIOCGABS(m->code), &ai) < 0 || ai.maximum <= ai.minimum)
			continue;	/* this revision has no such axis */
		ev->axes[ev->naxes].code = m->code;
		ev->axes[ev->naxes].axis = m->axis;
		ev->axes[ev->naxes].cal = (struct joy_axis_cal){
			.min = ai.minimum, .max = ai.maximum, .center = ai.value,
			.deadzone = ev->p->deadzone, .invert = m->invert,
		};
		/* a stick pushed at start would make a bad center */
		if (ai.value <= ai.minimum || ai.value >= ai.maximum)
			ev->axes[ev->naxes].cal.center = (ai.minimum + ai.maximum) / 2;
		LOGI(TAG, "%s: axis %u: %d..%d, center %d, flat %d", ev->p->name, m->code,
		     ai.minimum, ai.maximum, ev->axes[ev->naxes].cal.center, ai.flat);
		ev->naxes++;
	}
}

static void handle(struct evdev *ev, const struct input_event *e)
{
	int i;

	if (e->type == EV_KEY && e->value != 2) {	/* 2: kernel autorepeat */
		const struct evdev_key_map *k;

		for (k = ev->p->keys; k && k->code; k++)
			if (k->code == e->code) {
				input_report_button(k->button, e->value != 0);
				return;
			}
		LOGD(TAG, "%s: unmapped key %u", ev->p->name, e->code);
	} else if (e->type == EV_ABS) {
		for (i = 0; i < ev->naxes; i++)
			if (ev->axes[i].code == e->code) {
				input_report_axis(ev->axes[i].axis, joy_normalize(&ev->axes[i].cal, e->value));
				return;
			}
	}
}

static void on_readable(int fd, short revents, void *ud)
{
	struct evdev *ev = ud;
	struct input_event buf[32];
	ssize_t n;
	size_t i;

	if (revents & (POLLERR | POLLHUP)) {
		LOGW(TAG, "%s (%s) went away", ev->p->name, ev->path);
		mainloop_remove_fd(ev->ml, fd);
		return;
	}
	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		for (i = 0; i < (size_t)n / sizeof(buf[0]); i++)
			handle(ev, &buf[i]);
	}
	if (n < 0 && errno != EAGAIN)
		LOGW(TAG, "%s: read: %s", ev->path, strerror(errno));
}

static void *evdev_open(const void *params, struct mainloop *ml)
{
	struct evdev *ev = calloc(1, sizeof(*ev));

	if (!ev)
		return NULL;
	ev->ml = ml;
	ev->p = params;
	ev->fd = open_by_name(ev->p->name, ev->path, sizeof(ev->path));
	if (ev->fd < 0) {
		LOGW(TAG, "input device \"%s\" not found", ev->p->name);
		free(ev);
		return NULL;
	}
	setup_axes(ev);
	mainloop_add_fd(ml, ev->fd, POLLIN, on_readable, ev);
	LOGI(TAG, "%s: %s", ev->p->name, ev->path);
	return ev;
}

static void evdev_close(void *h)
{
	struct evdev *ev = h;

	mainloop_remove_fd(ev->ml, ev->fd);
	close(ev->fd);
	free(ev);
}

const struct hal_input_ops input_evdev = {
	.name = "evdev",
	.open = evdev_open,
	.close = evdev_close,
};
