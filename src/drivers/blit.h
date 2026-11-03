/*
 * Copy a rectangle of a logical (as the user sees it) XRGB/ARGB8888 buffer
 * to a scanout buffer in the panel's native orientation, turned like
 * fbcon=rotate:N. Shared by the fbdev and DRM displays.
 */
#pragma once

#include <stdint.h>

#include "hal/hal.h"

enum blit_mode {
	BLIT_COPY,		/* 32 bpp as is */
	BLIT_PREMULTIPLY,	/* 32 bpp, straight alpha -> premultiplied */
	BLIT_RGB565,		/* 16 bpp */
};

struct blit_dst {
	uint8_t *pixels;
	int width, height;	/* native panel size */
	int stride;		/* bytes */
};

void blit_rotated(const struct blit_dst *dst, const struct hal_fb *src,
		  const struct hal_rect *r, int rotation, enum blit_mode mode);

/*
 * dst = top (straight alpha) over the opaque bottom, a rectangle of three
 * logical buffers of the same size. Done in normal memory: scanout buffers
 * are often uncached, too slow to read back.
 */
void blit_compose(struct hal_fb *dst, const struct hal_fb *top, const struct hal_fb *bottom,
		  const struct hal_rect *r);

/* Damage list: up to BLIT_MAX_RECTS rectangles, merged into one box beyond that */
#define BLIT_MAX_RECTS 16

struct damage {
	struct hal_rect r[BLIT_MAX_RECTS];
	int n;
};

void damage_add(struct damage *d, const struct hal_rect *r, int width, int height);
