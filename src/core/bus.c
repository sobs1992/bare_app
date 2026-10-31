#include "bus.h"

#include <poll.h>

#include "log.h"
#include "mainloop.h"
#include "msgq.h"

#define MAX_SUBS 64
#define MAX_PER_ITER 64		/* a chain of events must not starve the loop */

struct sub {
	int id;			/* 0: free */
	enum ev_type type;
	bus_cb cb;
	void *ud;
};

static struct {
	struct mainloop *ml;
	struct msgq *q;
	struct sub subs[MAX_SUBS];
	int next_id;
} bus;

static void on_queue(int fd, short revents, void *ud)
{
	struct ev e;
	int n = 0, i;

	(void)fd;
	(void)revents;
	(void)ud;
	msgq_ack(bus.q);
	while (n < MAX_PER_ITER && msgq_pop(bus.q, &e)) {
		n++;
		for (i = 0; i < MAX_SUBS; i++)
			if (bus.subs[i].id && bus.subs[i].type == e.type)
				bus.subs[i].cb(&e, bus.subs[i].ud);
	}
	if (n == MAX_PER_ITER)
		msgq_kick(bus.q);
}

int bus_init(struct mainloop *ml)
{
	bus.ml = ml;
	bus.q = msgq_new(sizeof(struct ev), 128);
	if (!bus.q)
		return -1;
	return mainloop_add_fd(ml, msgq_fd(bus.q), POLLIN, on_queue, NULL);
}

void bus_deinit(void)
{
	if (!bus.q)
		return;
	mainloop_remove_fd(bus.ml, msgq_fd(bus.q));
	msgq_free(bus.q);
	bus.q = NULL;
}

void bus_post(const struct ev *e)
{
	msgq_post(bus.q, e);
}

int bus_subscribe(enum ev_type type, bus_cb cb, void *ud)
{
	int i;

	for (i = 0; i < MAX_SUBS; i++)
		if (!bus.subs[i].id) {
			bus.subs[i] = (struct sub){ ++bus.next_id, type, cb, ud };
			return bus.subs[i].id;
		}
	LOGE("bus", "too many subscribers");
	return -1;
}

void bus_unsubscribe(int id)
{
	int i;

	for (i = 0; i < MAX_SUBS; i++)
		if (bus.subs[i].id == id)
			bus.subs[i].id = 0;
}
