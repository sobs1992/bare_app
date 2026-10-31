#include "blit.h"

#include <stddef.h>

#include "core/util.h"

static inline uint32_t premultiply(uint32_t c)
{
	uint32_t a = c >> 24;

	if (a == 0xff)
		return c;
	if (a == 0)
		return 0;
	/* (x * a + 127) / 255 on the R/B and G lanes at once */
	uint32_t rb = (c & 0x00ff00ff) * a + 0x007f007f;
	uint32_t g = (c & 0x0000ff00) * a + 0x00007f00;

	rb = ((rb + ((rb >> 8) & 0x00ff00ff)) >> 8) & 0x00ff00ff;
	g = ((g + ((g >> 8) & 0x0000ff00)) >> 8) & 0x0000ff00;
	return (a << 24) | rb | g;
}

static inline uint16_t to_rgb565(uint32_t c)
{
	return ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f);
}

void blit_rotated(const struct blit_dst *dst, const struct hal_fb *src,
		  const struct hal_rect *r, int rotation, enum blit_mode mode)
{
	int bpp = mode == BLIT_RGB565 ? 2 : 4;
	int sx, sy, x, y;
	/* destination step per logical x and y, in bytes */
	ptrdiff_t dx, dy;
	uint8_t *row;

	/* logical (sx, sy) -> native (x, y), as fbcon=rotate:N */
	switch (rotation & 3) {
	case 1:  x = dst->width - 1 - r->y1; y = r->x1;
		 dx = dst->stride; dy = -bpp; break;
	case 2:  x = dst->width - 1 - r->x1; y = dst->height - 1 - r->y1;
		 dx = -bpp; dy = -(ptrdiff_t)dst->stride; break;
	case 3:  x = r->y1; y = dst->height - 1 - r->x1;
		 dx = -(ptrdiff_t)dst->stride; dy = bpp; break;
	default: x = r->x1; y = r->y1;
		 dx = bpp; dy = dst->stride; break;
	}
	row = dst->pixels + (ptrdiff_t)y * dst->stride + (ptrdiff_t)x * bpp;

	for (sy = r->y1; sy <= r->y2; sy++, row += dy) {
		const uint32_t *s = (const uint32_t *)((const uint8_t *)src->pixels +
						       (size_t)sy * src->stride) + r->x1;
		uint8_t *d = row;

		switch (mode) {
		case BLIT_COPY:
			for (sx = r->x1; sx <= r->x2; sx++, d += dx)
				*(uint32_t *)d = *s++;
			break;
		case BLIT_PREMULTIPLY:
			for (sx = r->x1; sx <= r->x2; sx++, d += dx)
				*(uint32_t *)d = premultiply(*s++);
			break;
		case BLIT_RGB565:
			for (sx = r->x1; sx <= r->x2; sx++, d += dx)
				*(uint16_t *)d = to_rgb565(*s++);
			break;
		}
	}
}

void damage_add(struct damage *d, const struct hal_rect *r, int width, int height)
{
	struct hal_rect c = {
		MAX(r->x1, 0), MAX(r->y1, 0), MIN(r->x2, width - 1), MIN(r->y2, height - 1),
	};
	int i;

	if (c.x1 > c.x2 || c.y1 > c.y2)
		return;
	if (d->n == BLIT_MAX_RECTS) {
		struct hal_rect *b = &d->r[0];

		for (i = 1; i < d->n; i++) {
			b->x1 = MIN(b->x1, d->r[i].x1);
			b->y1 = MIN(b->y1, d->r[i].y1);
			b->x2 = MAX(b->x2, d->r[i].x2);
			b->y2 = MAX(b->y2, d->r[i].y2);
		}
		d->n = 1;
	}
	d->r[d->n++] = c;
}
