/*
 * evdev input: finds /dev/input/eventN by device name (the number depends
 * on driver probe order) and maps its keys and axes to the logical gamepad.
 * Axes go to core/input over the range from EVIOCGABS (joy_linear()); the
 * value at open time is reported as the axis' rest position, the core
 * calibrates the sticks.
 *
 * Hotplug: a device that is missing at start or goes away is looked for
 * again on every "add" uevent of the input subsystem. Buttons held on a
 * device that disappears are released, its axes return to 0.
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
#include "core/uevent.h"

#define TAG "evdev"
#define MAX_AXES 8
#define RETRY_MS 100		/* mdev creates the node shortly after the uevent */
#define RETRIES 10

struct evdev {
	struct mainloop *ml;
	const struct evdev_params *p;
	int fd;			/* -1 while the device is absent */
	char path[32];
	struct {
		uint16_t code;
		enum gp_axis axis;
		struct joy_axis_cal cal;
		float rest;
	} axes[MAX_AXES];
	int naxes;
	float moved[GP_AXIS_COUNT];	/* until the SYN_REPORT */
	uint32_t moved_mask;
	uint32_t held;		/* GP_BIT() of buttons pressed on this device */
	struct uevent_mon *mon;
	struct ml_timer *retry;
	int retries;
};

/*
 * The name of eventN from sysfs: 1 = it is want, 0 = another one, -1 = no
 * sysfs. Opening a device to ask it starts it: an adc-joystick powers up
 * the SARADC for ~25 ms each time, and each lookup went through the
 * devices before the one it wanted.
 */
static int sysfs_name_is(int i, const char *want)
{
	char path[64], name[128];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "/sys/class/input/event%d/device/name", i);
	f = fopen(path, "re");
	if (!f)
		return -1;
	n = fread(name, 1, sizeof(name) - 1, f);
	fclose(f);
	while (n > 0 && name[n - 1] == '\n')
		n--;
	name[n] = '\0';
	return !strcmp(name, want);
}

static int open_by_name(const char *want, char *path, size_t size)
{
	char name[128];
	int i, fd;

	for (i = 0; i < 64; i++) {
		if (sysfs_name_is(i, want) == 0)
			continue;
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

	ev->naxes = 0;
	for (m = ev->p->axes; m && m->axis != GP_AXIS_COUNT && ev->naxes < MAX_AXES; m++) {
		struct input_absinfo ai;

		if (ioctl(ev->fd, EVIOCGABS(m->code), &ai) < 0 || ai.maximum <= ai.minimum)
			continue;	/* this revision has no such axis */
		ev->axes[ev->naxes].code = m->code;
		ev->axes[ev->naxes].axis = m->axis;
		ev->axes[ev->naxes].cal = (struct joy_axis_cal){
			.min = ai.minimum, .max = ai.maximum, .invert = m->invert,
		};
		/* a stick pushed at start would make a bad rest position */
		ev->axes[ev->naxes].rest = ai.value <= ai.minimum || ai.value >= ai.maximum ? 0 :
					   joy_linear(&ev->axes[ev->naxes].cal, ai.value);
		input_report_rest(m->axis, ev->axes[ev->naxes].rest);
		LOGI(TAG, "%s: axis %u: %d..%d, at rest %d, flat %d", ev->p->name, m->code,
		     ai.minimum, ai.maximum, ai.value, ai.flat);
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
				if (e->value)
					ev->held |= GP_BIT(k->button);
				else
					ev->held &= ~GP_BIT(k->button);
				input_report_button(k->button, e->value != 0);
				return;
			}
		LOGD(TAG, "%s: unmapped key %u", ev->p->name, e->code);
	} else if (e->type == EV_ABS) {
		for (i = 0; i < ev->naxes; i++)
			if (ev->axes[i].code == e->code) {
				ev->moved[ev->axes[i].axis] = joy_linear(&ev->axes[i].cal, e->value);
				ev->moved_mask |= GP_BIT(ev->axes[i].axis);
				return;
			}
	} else if (e->type == EV_SYN && e->code == SYN_REPORT && ev->moved_mask) {
		/* both axes of a stick at once: no half-moved diagonal */
		input_report_axes(ev->moved, ev->moved_mask);
		ev->moved_mask = 0;
	}
}

static void detach(struct evdev *ev)
{
	int b, i;

	if (ev->fd < 0)
		return;
	LOGW(TAG, "%s (%s) went away", ev->p->name, ev->path);
	mainloop_remove_fd(ev->ml, ev->fd);
	close(ev->fd);
	ev->fd = -1;
	for (b = 0; b < GP_COUNT; b++)
		if (ev->held & GP_BIT(b))
			input_report_button(b, false);
	ev->held = 0;
	ev->moved_mask = 0;
	for (i = 0; i < ev->naxes; i++)
		input_report_axis(ev->axes[i].axis, ev->axes[i].rest);
}

static void on_readable(int fd, short revents, void *ud)
{
	struct evdev *ev = ud;
	struct input_event buf[32];
	ssize_t n;
	size_t i;

	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		for (i = 0; i < (size_t)n / sizeof(buf[0]); i++)
			handle(ev, &buf[i]);
	}
	if ((n < 0 && errno != EAGAIN) || (revents & (POLLERR | POLLHUP)))
		detach(ev);	/* ENODEV after the device was removed */
}

/* Tell the input core which mapped keys and axes this device really has */
static void declare(struct evdev *ev)
{
	uint8_t keys[KEY_MAX / 8 + 1] = { 0 };
	const struct evdev_key_map *k;
	uint32_t buttons = 0, axes = 0;
	int i;

	if (ev->p->keys && ioctl(ev->fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0)
		for (k = ev->p->keys; k->code; k++)
			if (keys[k->code / 8] & (1 << (k->code % 8)))
				buttons |= GP_BIT(k->button);
	for (i = 0; i < ev->naxes; i++)
		axes |= GP_BIT(ev->axes[i].axis);
	input_declare(buttons, axes);
}

static bool attach(struct evdev *ev)
{
	ev->fd = open_by_name(ev->p->name, ev->path, sizeof(ev->path));
	if (ev->fd < 0)
		return false;
	setup_axes(ev);
	declare(ev);
	mainloop_add_fd(ev->ml, ev->fd, POLLIN, on_readable, ev);
	LOGI(TAG, "%s: %s", ev->p->name, ev->path);
	return true;
}

#ifdef CONFIG_INPUT_HOTPLUG
static void on_retry(void *ud)
{
	struct evdev *ev = ud;

	if (ev->fd >= 0 || attach(ev) || --ev->retries <= 0) {
		mainloop_timer_cancel(ev->ml, ev->retry);
		ev->retry = NULL;
	}
}

static void on_uevent(const struct uevent *u, void *ud)
{
	struct evdev *ev = ud;

	if (ev->fd >= 0 || strcmp(u->action, "add") || !u->devname ||
	    strncmp(u->devname, "input/event", 11))
		return;
	ev->retries = RETRIES;
	if (!ev->retry)
		ev->retry = mainloop_add_timer(ev->ml, RETRY_MS, RETRY_MS, on_retry, ev);
}
#endif

static void *evdev_open(const void *params, struct mainloop *ml)
{
	struct evdev *ev = calloc(1, sizeof(*ev));

	if (!ev)
		return NULL;
	ev->ml = ml;
	ev->p = params;
	ev->fd = -1;
#ifdef CONFIG_INPUT_HOTPLUG
	ev->mon = uevent_open(ml, "input", on_uevent, ev);
#endif
	if (!attach(ev)) {
		if (!ev->mon) {
			LOGW(TAG, "input device \"%s\" not found", ev->p->name);
			free(ev);
			return NULL;
		}
		LOGW(TAG, "input device \"%s\" not found, waiting for it", ev->p->name);
	}
	return ev;
}

static void evdev_close(void *h)
{
	struct evdev *ev = h;

#ifdef CONFIG_INPUT_HOTPLUG
	mainloop_timer_cancel(ev->ml, ev->retry);
	uevent_close(ev->mon);
#endif
	if (ev->fd >= 0) {
		mainloop_remove_fd(ev->ml, ev->fd);
		close(ev->fd);
	}
	free(ev);
}

const struct hal_input_ops input_evdev = {
	.name = "evdev",
	.open = evdev_open,
	.close = evdev_close,
};
