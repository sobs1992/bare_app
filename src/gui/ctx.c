#include "ctx.h"

#include <pthread.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static struct {
	pthread_mutex_t lock;
	struct gui_ctx live;
	uint32_t dirty;
	int fd;
} cx = { .lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1 };

int ctx_init(void)
{
	memset(&cx.live, 0, sizeof(cx.live));
	cx.live.power.capacity = -1;
	cx.live.audio.volume = -1;
	cx.live.nav.stack[0] = SCREEN_HOME;
	cx.live.nav.depth = 1;
	cx.dirty = CTX_ALL;	/* the first snapshot sees everything */
	cx.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	return cx.fd < 0 ? -1 : 0;
}

void ctx_deinit(void)
{
	if (cx.fd >= 0)
		close(cx.fd);
	cx.fd = -1;
}

struct gui_ctx *ctx_lock(void)
{
	pthread_mutex_lock(&cx.lock);
	return &cx.live;
}

void ctx_unlock(uint32_t changed)
{
	uint64_t one = 1;
	bool wake = changed && !cx.dirty;

	cx.dirty |= changed;
	pthread_mutex_unlock(&cx.lock);
	if (wake && write(cx.fd, &one, sizeof(one)) < 0) {
		/* the counter cannot overflow here; nothing to do */
	}
}

uint32_t ctx_snapshot(struct gui_ctx *snap)
{
	uint32_t changed;

	pthread_mutex_lock(&cx.lock);
	changed = cx.dirty;
	if (changed)
		*snap = cx.live;
	cx.dirty = 0;
	pthread_mutex_unlock(&cx.lock);
	return changed;
}

int ctx_fd(void)
{
	return cx.fd;
}

void ctx_ack(void)
{
	uint64_t v;

	if (read(cx.fd, &v, sizeof(v)) < 0) {
		/* EAGAIN: nothing pending */
	}
}
