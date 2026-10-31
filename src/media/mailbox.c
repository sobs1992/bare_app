#include "mailbox.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct mailbox {
	pthread_mutex_t lock;
	struct hal_fb buf[3];
	int writing;		/* producer's buffer */
	int ready;		/* newest published frame, -1 none */
	int reading;		/* consumer's buffer, -1 none */
	unsigned dropped;
	int fd;
};

struct mailbox *mailbox_new(int width, int height)
{
	struct mailbox *mb = calloc(1, sizeof(*mb));
	int i;

	if (!mb)
		return NULL;
	pthread_mutex_init(&mb->lock, NULL);
	mb->ready = -1;
	mb->reading = -1;
	mb->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	for (i = 0; i < 3; i++) {
		mb->buf[i].width = width;
		mb->buf[i].height = height;
		mb->buf[i].stride = width * 4;
		mb->buf[i].format = HAL_FMT_XRGB8888;
		mb->buf[i].pixels = calloc((size_t)width * height, 4);
	}
	if (mb->fd < 0 || !mb->buf[0].pixels || !mb->buf[1].pixels || !mb->buf[2].pixels) {
		mailbox_free(mb);
		return NULL;
	}
	return mb;
}

void mailbox_free(struct mailbox *mb)
{
	int i;

	if (!mb)
		return;
	for (i = 0; i < 3; i++)
		free(mb->buf[i].pixels);
	if (mb->fd >= 0)
		close(mb->fd);
	pthread_mutex_destroy(&mb->lock);
	free(mb);
}

struct hal_fb *mailbox_begin(struct mailbox *mb)
{
	int i;

	pthread_mutex_lock(&mb->lock);
	for (i = 0; i < 3; i++)
		if (i != mb->ready && i != mb->reading)
			break;
	mb->writing = i;	/* three buffers, two taken at most: always found */
	pthread_mutex_unlock(&mb->lock);
	return &mb->buf[i];
}

void mailbox_end(struct mailbox *mb)
{
	uint64_t one = 1;

	pthread_mutex_lock(&mb->lock);
	if (mb->ready >= 0)
		mb->dropped++;
	mb->ready = mb->writing;
	pthread_mutex_unlock(&mb->lock);
	if (write(mb->fd, &one, sizeof(one)) < 0) {
		/* counter overflow is impossible here */
	}
}

int mailbox_fd(struct mailbox *mb)
{
	return mb->fd;
}

void mailbox_ack(struct mailbox *mb)
{
	uint64_t v;

	if (read(mb->fd, &v, sizeof(v)) < 0) {
		/* EAGAIN: nothing pending */
	}
}

struct hal_fb *mailbox_take(struct mailbox *mb)
{
	struct hal_fb *fb = NULL;

	pthread_mutex_lock(&mb->lock);
	if (mb->ready >= 0) {
		mb->reading = mb->ready;
		mb->ready = -1;
		fb = &mb->buf[mb->reading];
	}
	pthread_mutex_unlock(&mb->lock);
	return fb;
}

unsigned mailbox_dropped(struct mailbox *mb)
{
	unsigned n;

	pthread_mutex_lock(&mb->lock);
	n = mb->dropped;
	pthread_mutex_unlock(&mb->lock);
	return n;
}
