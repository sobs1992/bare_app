#include "msgq.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "log.h"

struct msgq {
	pthread_mutex_t lock;
	size_t msg_size;
	unsigned capacity, head, count;
	int fd;
	unsigned dropped;
	unsigned char buf[];
};

struct msgq *msgq_new(size_t msg_size, unsigned capacity)
{
	struct msgq *q = calloc(1, sizeof(*q) + msg_size * capacity);

	if (!q)
		return NULL;
	q->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (q->fd < 0) {
		free(q);
		return NULL;
	}
	pthread_mutex_init(&q->lock, NULL);
	q->msg_size = msg_size;
	q->capacity = capacity;
	return q;
}

void msgq_free(struct msgq *q)
{
	if (!q)
		return;
	close(q->fd);
	pthread_mutex_destroy(&q->lock);
	free(q);
}

bool msgq_post(struct msgq *q, const void *msg)
{
	bool ok = false;

	pthread_mutex_lock(&q->lock);
	if (q->count < q->capacity) {
		unsigned tail = (q->head + q->count) % q->capacity;

		memcpy(q->buf + tail * q->msg_size, msg, q->msg_size);
		q->count++;
		ok = true;
	} else if (!q->dropped++) {
		LOGW("msgq", "queue full, dropping messages");
	}
	pthread_mutex_unlock(&q->lock);
	if (ok)
		msgq_kick(q);
	return ok;
}

bool msgq_pop(struct msgq *q, void *msg)
{
	bool ok = false;

	pthread_mutex_lock(&q->lock);
	if (q->count) {
		memcpy(msg, q->buf + q->head * q->msg_size, q->msg_size);
		q->head = (q->head + 1) % q->capacity;
		q->count--;
		ok = true;
	}
	pthread_mutex_unlock(&q->lock);
	return ok;
}

int msgq_fd(struct msgq *q)
{
	return q->fd;
}

void msgq_ack(struct msgq *q)
{
	uint64_t v;

	if (read(q->fd, &v, sizeof(v)) < 0) {
		/* EAGAIN: already acknowledged */
	}
}

void msgq_kick(struct msgq *q)
{
	uint64_t one = 1;

	if (write(q->fd, &one, sizeof(one)) < 0)
		LOGW("msgq", "eventfd write failed");
}
