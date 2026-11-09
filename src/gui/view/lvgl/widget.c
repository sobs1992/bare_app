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
