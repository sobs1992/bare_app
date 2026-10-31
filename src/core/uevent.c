#include "uevent.h"

#include <errno.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "mainloop.h"

#define TAG "uevent"

struct uevent_mon {
	struct mainloop *ml;
	int fd;
	char subsystem[32];
	uevent_cb cb;
	void *ud;
};

int uevent_parse(char *buf, int len, struct uevent *ev)
{
	char *p = buf, *end = buf + len;

	memset(ev, 0, sizeof(*ev));
	if (len <= 0)
		return -1;
	buf[len - 1] = '\0';
	/* kernel messages start with "action@devpath"; udev's (libudev) do not */
	if (!strchr(p, '@'))
		return -1;
	for (p += strlen(p) + 1; p < end; p += strlen(p) + 1) {
		if (!strncmp(p, "ACTION=", 7))
			ev->action = p + 7;
		else if (!strncmp(p, "SUBSYSTEM=", 10))
			ev->subsystem = p + 10;
		else if (!strncmp(p, "DEVPATH=", 8))
			ev->devpath = p + 8;
		else if (!strncmp(p, "DEVNAME=", 8))
			ev->devname = p + 8;
		else if (!strncmp(p, "POWER_SUPPLY_NAME=", 18))
			ev->name = p + 18;
		else if (!strncmp(p, "NAME=", 5) && !ev->name)
			ev->name = p + 5;	/* input devices: NAME="gpio-keys" */
	}
	return ev->action && ev->subsystem ? 0 : -1;
}

static void on_readable(int fd, short revents, void *ud)
{
	struct uevent_mon *m = ud;
	char buf[4096];
	struct uevent ev;
	ssize_t n;

	(void)revents;
	while ((n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		if (uevent_parse(buf, (int)n, &ev) || strcmp(ev.subsystem, m->subsystem))
			continue;
		LOGD(TAG, "%s %s %s", ev.action, ev.subsystem, ev.devname ? ev.devname : ev.devpath);
		m->cb(&ev, m->ud);
	}
	if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		LOGW(TAG, "recv: %s", strerror(errno));	/* ENOBUFS: events were lost */
}

struct uevent_mon *uevent_open(struct mainloop *ml, const char *subsystem, uevent_cb cb, void *ud)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };	/* kernel group */
	struct uevent_mon *m = calloc(1, sizeof(*m));

	if (!m)
		return NULL;
	m->fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if (m->fd < 0 || bind(m->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		LOGW(TAG, "netlink: %s, no hotplug events", strerror(errno));
		if (m->fd >= 0)
			close(m->fd);
		free(m);
		return NULL;
	}
	m->ml = ml;
	strncpy(m->subsystem, subsystem, sizeof(m->subsystem) - 1);
	m->cb = cb;
	m->ud = ud;
	mainloop_add_fd(ml, m->fd, POLLIN, on_readable, m);
	return m;
}

void uevent_close(struct uevent_mon *m)
{
	if (!m)
		return;
	mainloop_remove_fd(m->ml, m->fd);
	close(m->fd);
	free(m);
}
