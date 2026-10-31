/*
 * Layers: the UI with a transparent background over the video layer, which
 * shows a test pattern. Displays without a video layer get a message.
 */
#include <stdlib.h>

#include "../widget.h"

struct layers {
	bool video;
};

static void *layers_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct layers *l = calloc(1, sizeof(*l));
	lv_obj_t *scr, *panel, *text;

	(void)parent;
	if (!l)
		return NULL;
	l->video = view_video_set(VIDEO_TEST_PATTERN) == 0 && view_video_available();
	scr = lv_obj_create(NULL);
	if (l->video)
		lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
	statusbar_create(scr, LV_SYMBOL_IMAGE, tr(S_LAYERS));

	panel = lv_obj_create(scr);
	lv_obj_set_size(panel, LV_PCT(90), LV_SIZE_CONTENT);
	lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -12);
	lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(panel, LV_OPA_60, 0);
	lv_obj_set_style_border_width(panel, 0, 0);
	lv_obj_set_scrollable(panel, false);
	text = lv_label_create(panel);
	lv_obj_set_width(text, LV_PCT(100));
	lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_WRAP);
	if (l->video)
		lv_label_set_text(text, tr(S_LAYERS_VIDEO));
	else
		lv_label_set_text_fmt(text, LV_SYMBOL_WARNING "  %s", tr(S_LAYERS_NO_VIDEO));
	*root = scr;
	return l;
}

static void layers_deinit(void *self)
{
	view_video_set(VIDEO_NONE);
	free(self);
}

const struct widget_ops layers_screen = {
	.name = "layers",
	.deps = 0,
	.init = layers_init,
	.deinit = layers_deinit,
};
