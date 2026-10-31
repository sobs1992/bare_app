#include "testpattern.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/log.h"
#include "mailbox.h"

#define BOX 48

struct testpattern {
	struct mailbox *mb;
	pthread_t thread;
	atomic_bool stop;
	long period_ns;
	unsigned frame;
	int bx, by, vx, vy;
};

static const uint32_t bars[] = {
	0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x202020,
};

static void fill(struct hal_fb *fb, int x0, int y0, int w, int h, uint32_t c)
{
	int x, y;

	for (y = y0; y < y0 + h && y < fb->height; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)fb->pixels + (size_t)y * fb->stride);

		for (x = x0; x < x0 + w && x < fb->width; x++)
			row[x] = c;
	}
}

static void draw(struct testpattern *tp, struct hal_fb *fb)
{
	int n = sizeof(bars) / sizeof(bars[0]), bw = fb->width / n + 1, x, y;
	uint32_t *first = fb->pixels;

	/* bars scroll by two pixels per frame: build one row, copy it down */
	tp->frame++;
	for (x = 0; x < fb->width; x++)
		first[x] = bars[((x + tp->frame * 2) / bw) % n];
	for (y = 1; y < fb->height; y++)
		memcpy((uint8_t *)fb->pixels + (size_t)y * fb->stride, first, (size_t)fb->width * 4);
	tp->bx += tp->vx;
	tp->by += tp->vy;
	if (tp->bx < 0 || tp->bx > fb->width - BOX)
		tp->vx = -tp->vx, tp->bx += 2 * tp->vx;
	if (tp->by < 0 || tp->by > fb->height - BOX)
		tp->vy = -tp->vy, tp->by += 2 * tp->vy;
	fill(fb, tp->bx, tp->by, BOX, BOX, 0x000000);
	fill(fb, tp->bx + 6, tp->by + 6, BOX - 12, BOX - 12, 0xffffff);
}

static void *producer(void *arg)
{
	struct testpattern *tp = arg;
	struct timespec next;

	clock_gettime(CLOCK_MONOTONIC, &next);
	while (!atomic_load(&tp->stop)) {
		draw(tp, mailbox_begin(tp->mb));
		mailbox_end(tp->mb);
		next.tv_nsec += tp->period_ns;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	return NULL;
}

struct testpattern *testpattern_start(struct mailbox *mb, int fps)
{
	struct testpattern *tp = calloc(1, sizeof(*tp));
	sigset_t all, old;
	int err;

	if (!tp)
		return NULL;
	tp->mb = mb;
	tp->period_ns = 1000000000L / (fps > 0 ? fps : 30);
	tp->bx = 20;
	tp->by = 20;
	tp->vx = 3;
	tp->vy = 2;
	/* signals belong to the control thread */
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &old);
	err = pthread_create(&tp->thread, NULL, producer, tp);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	if (err) {
		LOGE("testpattern", "pthread_create: %s", strerror(err));
		free(tp);
		return NULL;
	}
	return tp;
}

void testpattern_stop(struct testpattern *tp)
{
	if (!tp)
		return;
	atomic_store(&tp->stop, true);
	pthread_join(tp->thread, NULL);
	free(tp);
}
