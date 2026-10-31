#include "display_mem.h"

#include <stdlib.h>

#include "core/log.h"
#include "shot.h"

#define TAG "memdisp"

struct memlayer {
	struct hal_layer base;
	struct hal_fb fb;
	bool present;
	unsigned frames;
};

struct memdisp {
	struct hal_display base;
	struct memlayer layers[LAYER_COUNT];
};

static int layer_init(struct memdisp *m, enum layer_role role, const struct mem_display_params *p,
		      enum hal_format format)
{
	struct memlayer *l = &m->layers[role];

	l->base.display = &m->base;
	l->base.role = role;
	l->fb.width = p->width;
	l->fb.height = p->height;
	l->fb.stride = p->width * 4;
	l->fb.format = format;
	l->fb.pixels = calloc((size_t)p->width * p->height, 4);
	l->present = l->fb.pixels != NULL;
	return l->present ? 0 : -1;
}

static void mem_free(struct memdisp *m)
{
	int i;

	for (i = 0; i < LAYER_COUNT; i++)
		free(m->layers[i].fb.pixels);
	free(m);
}

static struct hal_display *mem_open(const void *params, struct mainloop *ml)
{
	const struct mem_display_params *p = params;
	struct memdisp *m;

	(void)ml;
	m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->base.ops = &display_mem;
	if (layer_init(m, LAYER_UI, p, p->video_layer ? HAL_FMT_ARGB8888 : HAL_FMT_XRGB8888) ||
	    (p->video_layer && layer_init(m, LAYER_VIDEO, p, HAL_FMT_XRGB8888))) {
		mem_free(m);
		return NULL;
	}
	LOGI(TAG, "%dx%d in memory%s", p->width, p->height, p->video_layer ? ", video layer" : "");
	return &m->base;
}

static void mem_close(struct hal_display *d)
{
	struct memdisp *m = (struct memdisp *)d;

	LOGI(TAG, "%u UI frames, %u video frames", m->layers[LAYER_UI].frames,
	     m->layers[LAYER_VIDEO].frames);
	mem_free(m);
}

#ifdef CONFIG_SNAPSHOT
static int mem_snapshot(struct hal_display *d, const char *path)
{
	struct memdisp *m = (struct memdisp *)d;

	return shot_write_ppm(path, &m->layers[LAYER_UI].fb,
			      m->layers[LAYER_VIDEO].present ? &m->layers[LAYER_VIDEO].fb : NULL);
}
#endif

static void mem_info(struct hal_display *d, struct hal_display_info *info)
{
	struct memdisp *m = (struct memdisp *)d;

	info->width = m->layers[LAYER_UI].fb.width;
	info->height = m->layers[LAYER_UI].fb.height;
	info->rotation = HAL_ROT_0;
}

static struct hal_layer *mem_layer(struct hal_display *d, enum layer_role role)
{
	struct memdisp *m = (struct memdisp *)d;

	return m->layers[role].present ? &m->layers[role].base : NULL;
}

static struct hal_fb *mem_acquire(struct hal_layer *l)
{
	return &((struct memlayer *)l)->fb;
}

static void mem_damage(struct hal_layer *l, const struct hal_rect *r)
{
	(void)l;
	(void)r;
}

static void mem_submit(struct hal_layer *l)
{
	((struct memlayer *)l)->frames++;
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
#ifdef CONFIG_SNAPSHOT
	.snapshot = mem_snapshot,
#endif
};
