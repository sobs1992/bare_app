#include "display_mem.h"

#include <stdio.h>
#include <stdlib.h>

#include "core/log.h"

#define TAG "memdisp"

struct memdisp {
	struct hal_display base;
	struct hal_layer layer;
	struct hal_fb fb;
	unsigned frames;
};

static struct hal_display *mem_open(const void *params, struct mainloop *ml)
{
	const struct mem_display_params *p = params;
	struct memdisp *m;

	(void)ml;
	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->base.ops = &display_mem;
	m->layer.display = &m->base;
	m->layer.role = LAYER_UI;
	m->fb.width = p->width;
	m->fb.height = p->height;
	m->fb.stride = p->width * 4;
	m->fb.pixels = calloc((size_t)p->width * p->height, 4);
	if (!m->fb.pixels) {
		free(m);
		return NULL;
	}
	LOGI(TAG, "%dx%d in memory", p->width, p->height);
	return &m->base;
}

static void write_ppm(const struct memdisp *m, const char *path)
{
	FILE *f = fopen(path, "wb");
	int i, n = m->fb.width * m->fb.height;

	if (!f) {
		LOGE(TAG, "%s: cannot write", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", m->fb.width, m->fb.height);
	for (i = 0; i < n; i++) {
		uint32_t c = m->fb.pixels[i];

		fputc((c >> 16) & 0xff, f);
		fputc((c >> 8) & 0xff, f);
		fputc(c & 0xff, f);
	}
	fclose(f);
	LOGI(TAG, "%u frames, last one saved to %s", m->frames, path);
}

static void mem_close(struct hal_display *d)
{
	struct memdisp *m = (struct memdisp *)d;
	const char *shot = getenv("BARE_APP_SHOT");

	if (shot && *shot)
		write_ppm(m, shot);
	free(m->fb.pixels);
	free(m);
}

static void mem_info(struct hal_display *d, struct hal_display_info *info)
{
	struct memdisp *m = (struct memdisp *)d;

	info->width = m->fb.width;
	info->height = m->fb.height;
	info->rotation = HAL_ROT_0;
}

static struct hal_layer *mem_layer(struct hal_display *d, enum layer_role role)
{
	return role == LAYER_UI ? &((struct memdisp *)d)->layer : NULL;
}

static struct hal_fb *mem_acquire(struct hal_layer *l)
{
	return &((struct memdisp *)l->display)->fb;
}

static void mem_damage(struct hal_layer *l, const struct hal_rect *r)
{
	(void)l;
	(void)r;
}

static void mem_submit(struct hal_layer *l)
{
	((struct memdisp *)l->display)->frames++;
}

const struct hal_display_ops display_mem = {
	.name = "mem",
	.open = mem_open,
	.close = mem_close,
	.info = mem_info,
	.layer = mem_layer,
	.layer_acquire = mem_acquire,
	.layer_damage = mem_damage,
	.layer_submit = mem_submit,
};
