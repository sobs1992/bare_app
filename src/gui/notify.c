#include "notify.h"

#include <stddef.h>

#include "core/msgq.h"

static struct msgq *q;

int notify_init(void)
{
	q = msgq_new(sizeof(struct gui_notify), 32);
	return q ? 0 : -1;
}

void notify_deinit(void)
{
	msgq_free(q);
	q = NULL;
}

void gui_notify_post(enum notify_kind kind, int value)
{
	struct gui_notify n = { kind, value };

	msgq_post(q, &n);
}

bool notify_pop(struct gui_notify *n)
{
	return msgq_pop(q, n);
}

int notify_fd(void)
{
	return msgq_fd(q);
}

void notify_ack(void)
{
	msgq_ack(q);
}
