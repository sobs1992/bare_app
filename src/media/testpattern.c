#include "testpattern.h"

#include <stdlib.h>
#include <string.h>

#include "core/mainloop.h"
#include "hal/hal.h"

#define BOX 48

struct testpattern {
	struct mainloop *ml;
	struct hal_layer *layer;
	struct ml_timer *timer;
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

static void draw(void *ud)
{
	struct testpattern *tp = ud;
	struct hal_fb *fb = hal_layer_acquire(tp->layer);
	struct hal_rect all = { 0, 0, fb->width - 1, fb->height - 1 };
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
	hal_layer_damage(tp->layer, &all);
	hal_layer_submit(tp->layer);
}

struct testpattern *testpattern_start(struct mainloop *ml, struct hal_layer *layer, int fps)
{
	struct testpattern *tp = calloc(1, sizeof(*tp));

	if (!tp)
		return NULL;
	tp->ml = ml;
	tp->layer = layer;
	tp->bx = 20;
	tp->by = 20;
	tp->vx = 3;
	tp->vy = 2;
	tp->timer = mainloop_add_timer(ml, 0, 1000 / (fps > 0 ? fps : 30), draw, tp);
	return tp;
}

void testpattern_stop(struct testpattern *tp)
{
	struct hal_fb *fb;
	struct hal_rect all;

	if (!tp)
		return;
	mainloop_timer_cancel(tp->ml, tp->timer);
	fb = hal_layer_acquire(tp->layer);
	all = (struct hal_rect){ 0, 0, fb->width - 1, fb->height - 1 };
	memset(fb->pixels, 0, (size_t)fb->stride * fb->height);
	hal_layer_damage(tp->layer, &all);
	hal_layer_submit(tp->layer);
	free(tp);
}
