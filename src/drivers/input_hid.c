/*
 * USB (or Bluetooth) keyboards and mice: every /dev/input/eventN that has
 * letter keys or a relative X/Y with a left button, whatever its name,
 * plugged in at start or later (uevents of the input subsystem). Their
 * keys and moves go to core/input (input_report_key(), input_report_mouse_*()),
 * which hands them to a game as they are or turns keys into gamepad buttons.
 *
 * The devices are grabbed (EVIOCGRAB): the kernel's console would
 * otherwise take the keys too (Alt+F2 switches the virtual terminal away
 * from bare_app, Ctrl+Alt+Del reboots). A device that goes away has its
 * keys and buttons released.
 */
#include "input_hid.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/input.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "core/uevent.h"

#define TAG "hid"
#define MAX_DEVS 8
#define MAX_NODES 64
#define RETRY_MS 100		/* mdev creates the node shortly after the uevent */
#define RETRIES 10
#define TRACKED_KEYS 256

struct hid_dev {
	struct hid *hid;
	int fd;			/* -1: free slot */
	int node;		/* N of /dev/input/eventN */
	bool keyboard, mouse;
	uint8_t down[TRACKED_KEYS / 8];
	uint8_t buttons;	/* mouse: bit per button down */
	int dx, dy, wheel;	/* until the SYN_REPORT */
};

struct hid {
	struct mainloop *ml;
	struct hid_dev devs[MAX_DEVS];
	struct uevent_mon *mon;
	struct ml_timer *retry;
	int retries;
};

static bool has_bit(const uint8_t *bits, unsigned n)
{
	return bits[n / 8] >> (n % 8) & 1;
}

static int mouse_button(unsigned code)
{
	return code == BTN_LEFT ? 0 : code == BTN_RIGHT ? 1 : code == BTN_MIDDLE ? 2 : -1;
}

static void release_all(struct hid_dev *d)
{
	for (unsigned k = 0; k < TRACKED_KEYS; k++)
		if (has_bit(d->down, k))
			input_report_key(k, false);
	for (int b = 0; b < 8; b++)
		if (d->buttons & 1u << b)
			input_report_mouse_button(b, false);
	memset(d->down, 0, sizeof(d->down));
	d->buttons = 0;
	d->dx = d->dy = d->wheel = 0;
}

static void detach(struct hid_dev *d)
{
	if (d->fd < 0)
		return;
	LOGI(TAG, "event%d went away", d->node);
	mainloop_remove_fd(d->hid->ml, d->fd);
	close(d->fd);
	d->fd = -1;
	release_all(d);
}

static void handle(struct hid_dev *d, const struct input_event *e)
{
	int b;

	if (e->type == EV_KEY && e->value != 2) {	/* 2: the kernel's autorepeat */
		bool pressed = e->value != 0;

		if ((b = mouse_button(e->code)) >= 0) {
			if (pressed)
				d->buttons |= 1u << b;
			else
				d->buttons &= ~(1u << b);
			input_report_mouse_button(b, pressed);
		} else if (e->code < TRACKED_KEYS) {
			if (pressed)
				d->down[e->code / 8] |= 1u << (e->code % 8);
			else
				d->down[e->code / 8] &= ~(1u << (e->code % 8));
			input_report_key(e->code, pressed);
		}
	} else if (e->type == EV_REL) {
		if (e->code == REL_X)
			d->dx += e->value;
		else if (e->code == REL_Y)
			d->dy += e->value;
		else if (e->code == REL_WHEEL)
			d->wheel += e->value;
	} else if (e->type == EV_SYN && e->code == SYN_REPORT && (d->dx || d->dy || d->wheel)) {
		input_report_mouse_move(d->dx, d->dy, d->wheel);
		d->dx = d->dy = d->wheel = 0;
	}
}

static void on_readable(int fd, short revents, void *ud)
{
	struct hid_dev *d = ud;
	struct input_event buf[64];
	ssize_t n;

	for (;;) {
		n = read(fd, buf, sizeof(buf));
		if (n <= 0)
			break;
		for (size_t i = 0; i < (size_t)n / sizeof(buf[0]); i++)
			handle(d, &buf[i]);
	}
	if ((n < 0 && errno != EAGAIN) || (revents & (POLLERR | POLLHUP)))
		detach(d);	/* ENODEV once it is unplugged */
}

/* A keyboard has letters, a mouse moves on X and Y and has a left button */
static void classify(int fd, bool *keyboard, bool *mouse)
{
	uint8_t keys[KEY_MAX / 8 + 1] = { 0 }, rel[REL_MAX / 8 + 1] = { 0 };

	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
	ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
	*keyboard = has_bit(keys, KEY_A) && has_bit(keys, KEY_Z) && has_bit(keys, KEY_SPACE) &&
		    has_bit(keys, KEY_ENTER);
	*mouse = has_bit(rel, REL_X) && has_bit(rel, REL_Y) && has_bit(keys, BTN_LEFT);
}

static bool open_node(struct hid *h, int node)
{
	struct hid_dev *d = NULL;
	char path[32], name[128] = "";
	bool keyboard, mouse;
	int fd, i;

	for (i = 0; i < MAX_DEVS; i++) {
		if (h->devs[i].fd >= 0 && h->devs[i].node == node)
			return false;	/* ours already */
		if (h->devs[i].fd < 0 && !d)
			d = &h->devs[i];
	}
	snprintf(path, sizeof(path), "/dev/input/event%d", node);
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return false;
	classify(fd, &keyboard, &mouse);
	if ((!keyboard && !mouse) || !d) {
		close(fd);
		return false;
	}
	ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
	if (ioctl(fd, EVIOCGRAB, 1) < 0)
		LOGW(TAG, "%s: not grabbed: %s", path, strerror(errno));
	memset(d, 0, sizeof(*d));
	d->hid = h;
	d->fd = fd;
	d->node = node;
	d->keyboard = keyboard;
	d->mouse = mouse;
	mainloop_add_fd(h->ml, fd, POLLIN, on_readable, d);
	LOGI(TAG, "%s: \"%s\"%s%s", path, name, keyboard ? " keyboard" : "", mouse ? " mouse" : "");
	return true;
}

/* true if something new was found */
static bool scan(struct hid *h)
{
	bool found = false;

	for (int node = 0; node < MAX_NODES; node++)
		found |= open_node(h, node);
	return found;
}

#ifdef CONFIG_INPUT_HOTPLUG
static void on_retry(void *ud)
{
	struct hid *h = ud;

	if (scan(h) || --h->retries <= 0) {
		mainloop_timer_cancel(h->ml, h->retry);
		h->retry = NULL;
	}
}

static void on_uevent(const struct uevent *u, void *ud)
{
	struct hid *h = ud;

	if (strcmp(u->action, "add") || !u->devname || strncmp(u->devname, "input/event", 11))
		return;
	h->retries = RETRIES;
	if (!h->retry)
		h->retry = mainloop_add_timer(h->ml, RETRY_MS, RETRY_MS, on_retry, h);
}
#endif

static void *hid_open(const void *params, struct mainloop *ml)
{
	struct hid *h = calloc(1, sizeof(*h));

	(void)params;
	if (!h)
		return NULL;
	h->ml = ml;
	for (int i = 0; i < MAX_DEVS; i++)
		h->devs[i].fd = -1;
#ifdef CONFIG_INPUT_HOTPLUG
	h->mon = uevent_open(ml, "input", on_uevent, h);
#endif
	scan(h);
	return h;
}

static void hid_close(void *p)
{
	struct hid *h = p;

#ifdef CONFIG_INPUT_HOTPLUG
	mainloop_timer_cancel(h->ml, h->retry);
	uevent_close(h->mon);
#endif
	for (int i = 0; i < MAX_DEVS; i++)
		if (h->devs[i].fd >= 0) {
			mainloop_remove_fd(h->ml, h->devs[i].fd);
			close(h->devs[i].fd);
		}
	free(h);
}

const struct hal_input_ops input_hid = {
	.name = "hid",
	.open = hid_open,
	.close = hid_close,
};
