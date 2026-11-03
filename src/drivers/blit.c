#include "blit.h"

#include <stdbool.h>
#include <stddef.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

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

/* c (straight alpha) over the opaque pixel d */
static inline uint32_t over(uint32_t c, uint32_t d)
{
	uint32_t a = c >> 24, na = 0xff - a;

	if (a == 0xff)
		return c;
	if (a == 0)
		return d | 0xff000000;
	/* premultiplied c + d * (255 - a) / 255, R/B and G lanes at once */
	uint32_t rb = (d & 0x00ff00ff) * na + 0x007f007f;
	uint32_t g = (d & 0x0000ff00) * na + 0x00007f00;

	rb = ((rb + ((rb >> 8) & 0x00ff00ff)) >> 8) & 0x00ff00ff;
	g = ((g + ((g >> 8) & 0x0000ff00)) >> 8) & 0x0000ff00;
	return 0xff000000 | ((premultiply(c) & 0x00ffffff) + (rb | g));
}

static inline uint16_t to_rgb565(uint32_t c)
{
	return ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f);
}

/*
 * Square tiles for a turned copy: the source rows a tile reads stay in the
 * cache while it is written
 */
#define BLIT_TILE 16

/* n pixels from s (step apart) into d, converted */
static void convert(uint8_t *d, const uint32_t *s, ptrdiff_t step, int n, enum blit_mode mode)
{
	int x;

	switch (mode) {
	case BLIT_COPY:
		for (x = 0; x < n; x++, s += step)
			((uint32_t *)d)[x] = *s;
		break;
	case BLIT_PREMULTIPLY:
		for (x = 0; x < n; x++, s += step)
			((uint32_t *)d)[x] = premultiply(*s);
		break;
	case BLIT_RGB565:
		for (x = 0; x < n; x++, s += step)
			((uint16_t *)d)[x] = to_rgb565(*s);
		break;
	}
}

#ifdef __ARM_NEON
/*
 * A turned copy of 4 x 4 pixels: four source rows in, transposed in
 * registers, four native rows out. Source row k (s + k * step) holds native
 * column k; its pixels run along native y by 'down' (+1 or -1).
 */
static inline void turn4(uint8_t *d, ptrdiff_t stride, const uint32_t *s, ptrdiff_t step, ptrdiff_t down)
{
	const uint32_t *p = down > 0 ? s : s - 3;
	uint32x4_t r0 = vld1q_u32(p), r1 = vld1q_u32(p + step);
	uint32x4_t r2 = vld1q_u32(p + 2 * step), r3 = vld1q_u32(p + 3 * step);
	uint32x4_t t0 = vtrn1q_u32(r0, r1), t1 = vtrn2q_u32(r0, r1);
	uint32x4_t t2 = vtrn1q_u32(r2, r3), t3 = vtrn2q_u32(r2, r3);
	uint32x4_t c[4] = {
		vreinterpretq_u32_u64(vtrn1q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2))),
		vreinterpretq_u32_u64(vtrn1q_u64(vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3))),
		vreinterpretq_u32_u64(vtrn2q_u64(vreinterpretq_u64_u32(t0), vreinterpretq_u64_u32(t2))),
		vreinterpretq_u32_u64(vtrn2q_u64(vreinterpretq_u64_u32(t1), vreinterpretq_u64_u32(t3))),
	};
	int j;

	/* lane j of the loads is native row j (down > 0) or 3 - j */
	for (j = 0; j < 4; j++)
		vst1q_u32((uint32_t *)(d + (down > 0 ? j : 3 - j) * stride), c[j]);
}
#endif

/*
 * Copied in native order, row by row: the source is read in whatever
 * order that needs, the turned ones in tiles
 */
void blit_rotated(const struct blit_dst *dst, const struct hal_fb *src,
		  const struct hal_rect *r, int rotation, enum blit_mode mode)
{
	const ptrdiff_t sp = src->stride / 4;	/* source pixels per row */
	int bpp = mode == BLIT_RGB565 ? 2 : 4;
	int nx1, ny1, nx2, ny2;			/* the native box */
	const uint32_t *origin;			/* source pixel for native (nx1, ny1) */
	ptrdiff_t step, down;			/* source pixels per native x+1, y+1 */
	int tile_w, tx, ty, y;

	/* logical (sx, sy) -> native, as fbcon=rotate:N */
	switch (rotation & 3) {
	case 1:		/* nx = w - 1 - sy, ny = sx */
		nx1 = dst->width - 1 - r->y2; nx2 = dst->width - 1 - r->y1;
		ny1 = r->x1; ny2 = r->x2;
		origin = src->pixels + r->y2 * sp + r->x1;
		step = -sp; down = 1;
		break;
	case 2:		/* nx = w - 1 - sx, ny = h - 1 - sy */
		nx1 = dst->width - 1 - r->x2; nx2 = dst->width - 1 - r->x1;
		ny1 = dst->height - 1 - r->y2; ny2 = dst->height - 1 - r->y1;
		origin = src->pixels + r->y2 * sp + r->x2;
		step = -1; down = -sp;
		break;
	case 3:		/* nx = sy, ny = h - 1 - sx */
		nx1 = r->y1; nx2 = r->y2;
		ny1 = dst->height - 1 - r->x2; ny2 = dst->height - 1 - r->x1;
		origin = src->pixels + r->y1 * sp + r->x2;
		step = sp; down = -1;
		break;
	default:
		nx1 = r->x1; nx2 = r->x2;
		ny1 = r->y1; ny2 = r->y2;
		origin = src->pixels + r->y1 * sp + r->x1;
		step = 1; down = sp;
		break;
	}
	/* unturned rows are read in order anyway: no tiles across them */
	tile_w = step == 1 || step == -1 ? nx2 - nx1 + 1 : BLIT_TILE;

	for (ty = ny1; ty <= ny2; ty += BLIT_TILE) {
		int ey = MIN(ty + BLIT_TILE - 1, ny2);

		for (tx = nx1; tx <= nx2; tx += tile_w) {
			int n = MIN(tile_w, nx2 - tx + 1);

			/*
			 * Turned, the tile reads its source a row apart, which the
			 * prefetcher does not follow: ask for the next tile's now
			 */
			if (tile_w == BLIT_TILE && tx + tile_w <= nx2) {
				const uint32_t *next = origin + (ty - ny1) * down + (tx + tile_w - nx1) * step;
				int k, m = MIN(BLIT_TILE, nx2 - tx - tile_w + 1);

				for (k = 0; k < m; k++) {
					__builtin_prefetch(next + k * step);
					__builtin_prefetch(next + k * step + (ey - ty) * down);
				}
			}
			y = ty;
#ifdef __ARM_NEON
			/* turned copies: whole 4 x 4 blocks in registers */
			if (mode == BLIT_COPY && (down == 1 || down == -1)) {
				for (; y + 3 <= ey; y += 4) {
					const uint32_t *s = origin + (y - ny1) * down + (tx - nx1) * step;
					uint8_t *d = dst->pixels + (ptrdiff_t)y * dst->stride + (ptrdiff_t)tx * 4;
					int x, k;

					for (x = 0; x + 3 < n; x += 4)
						turn4(d + x * 4, dst->stride, s + x * step, step, down);
					for (k = 0; k < 4 && x < n; k++)
						convert(d + (ptrdiff_t)k * dst->stride + x * 4,
							s + k * down + x * step, step, n - x, mode);
				}
			}
#endif
			for (; y <= ey; y++)
				convert(dst->pixels + (ptrdiff_t)y * dst->stride + (ptrdiff_t)tx * bpp,
					origin + (y - ny1) * down + (tx - nx1) * step, step, n, mode);
		}
	}
}

void blit_compose(struct hal_fb *dst, const struct hal_fb *top, const struct hal_fb *bottom,
		  const struct hal_rect *r)
{
	int x, y;

	for (y = r->y1; y <= r->y2; y++) {
		uint32_t *d = (uint32_t *)((uint8_t *)dst->pixels + (size_t)y * dst->stride);
		const uint32_t *t = (const uint32_t *)((const uint8_t *)top->pixels + (size_t)y * top->stride);
		const uint32_t *b = (const uint32_t *)((const uint8_t *)bottom->pixels +
						       (size_t)y * bottom->stride);

		for (x = r->x1; x <= r->x2; x++)
			d[x] = over(t[x], b[x]);
	}
}

static bool covers(const struct hal_rect *a, const struct hal_rect *b)
{
	return a->x1 <= b->x1 && a->y1 <= b->y1 && a->x2 >= b->x2 && a->y2 >= b->y2;
}

void damage_add(struct damage *d, const struct hal_rect *r, int width, int height)
{
	struct hal_rect c = {
		MAX(r->x1, 0), MAX(r->y1, 0), MIN(r->x2, width - 1), MIN(r->y2, height - 1),
	};
	int i;

	if (c.x1 > c.x2 || c.y1 > c.y2)
		return;
	/*
	 * A buffer collects the damage of the frames it missed: often the same
	 * area (a whole video frame) twice. What another rectangle covers
	 * would only be copied again.
	 */
	for (i = 0; i < d->n; i++)
		if (covers(&d->r[i], &c))
			return;
	for (i = 0; i < d->n;) {
		if (covers(&c, &d->r[i]))
			d->r[i] = d->r[--d->n];
		else
			i++;
	}
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
