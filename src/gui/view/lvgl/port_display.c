/*
 * LVGL renders straight into the HAL layer buffer (direct mode: the buffer
 * keeps its content, LVGL redraws only invalidated areas). Each flushed
 * area becomes HAL damage; the last one of a frame submits it, and the
 * driver takes care of rotation.
 */
#include "lvgl_view.h"

#include "core/log.h"
#include "hal/hal.h"

#define TAG "lvgl"

static struct hal_layer *layer;

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	struct hal_rect r = { area->x1, area->y1, area->x2, area->y2 };

	(void)px_map;
	hal_layer_damage(layer, &r);
	if (lv_display_flush_is_last(disp))
		hal_layer_submit(layer);
	lv_display_flush_ready(disp);
}

lv_display_t *port_display_init(struct hal_display *d)
{
	struct hal_display_info info;
	lv_display_t *disp;
	struct hal_fb *fb;

	layer = hal_display_layer(d, LAYER_UI);
	if (!layer) {
		LOGE(TAG, "display has no UI layer");
		return NULL;
	}
	hal_display_info(d, &info);
	fb = hal_layer_acquire(layer);
	if (fb->width != info.width || fb->height != info.height || fb->stride != fb->width * 4) {
		LOGE(TAG, "unexpected UI buffer %dx%d stride %d", fb->width, fb->height, fb->stride);
		return NULL;
	}
	disp = lv_display_create(info.width, info.height);
	if (!disp)
		return NULL;
	/* with alpha the layers below (video) show through transparent pixels */
	lv_display_set_color_format(disp, fb->format == HAL_FMT_ARGB8888 ? LV_COLOR_FORMAT_ARGB8888
									  : LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, fb->pixels, NULL, (uint32_t)fb->stride * fb->height,
			       LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush);
	LOGI(TAG, "display %dx%d %s (rotation %d done by the driver)", info.width, info.height,
	     fb->format == HAL_FMT_ARGB8888 ? "ARGB8888" : "XRGB8888", info.rotation);
	return disp;
}
