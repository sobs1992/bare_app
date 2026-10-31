#include "widget.h"

#include <stdlib.h>

#include "core/log.h"

struct widget {
	const struct widget_ops *ops;
	void *self;
	int sub;
};

static void on_delete(lv_event_t *e)
{
	struct widget *w = lv_event_get_user_data(e);

	if (w->sub > 0)
		view_unsubscribe(w->sub);
	if (w->ops->deinit)
		w->ops->deinit(w->self);
	free(w);
}

lv_obj_t *widget_create(const struct widget_ops *ops, lv_obj_t *parent)
{
	struct widget *w = calloc(1, sizeof(*w));
	lv_obj_t *root = NULL;

	if (!w)
		return NULL;
	w->ops = ops;
	w->self = ops->init(parent, &root);
	if (!root) {
		LOGE("widget", "%s: init failed", ops->name);
		free(w);
		return NULL;
	}
	lv_obj_set_user_data(root, w);
	lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, w);
	if (ops->deps && ops->update)
		w->sub = view_subscribe(ops->deps, ops->update, w->self);	/* first update */
	return root;
}

void *widget_self(lv_obj_t *root)
{
	struct widget *w = lv_obj_get_user_data(root);

	return w ? w->self : NULL;
}

lv_obj_t *screen_body(lv_obj_t *screen)
{
	lv_obj_t *body = lv_obj_create(screen);

	lv_obj_remove_style_all(body);
	lv_obj_set_size(body, LV_PCT(100), lv_display_get_vertical_resolution(lv_display_get_default()) - UI_STATUSBAR_H);
	lv_obj_set_pos(body, 0, UI_STATUSBAR_H);
	lv_obj_set_style_pad_all(body, 10, 0);
	lv_obj_set_style_pad_row(body, 10, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return body;
}
