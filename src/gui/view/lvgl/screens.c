/*
 * Screen manager: follows CTX_NAV. Each screen gets its own focus group,
 * so the focus never lands on objects of the screen being replaced.
 */
#include "lvgl_view.h"

#include "core/log.h"
#include "widget.h"

static const struct widget_ops *const screen_ops[SCREEN_COUNT] = {
	[SCREEN_HOME] = &home_screen,
	[SCREEN_INPUT_TEST] = &input_test_screen,
	[SCREEN_SETTINGS] = &settings_screen,
};

static struct {
	int sub;
	int shown;		/* enum screen_id, -1 none */
	lv_obj_t *screen;
	lv_group_t *group;
} sm = { .shown = -1 };

static void show(enum screen_id id)
{
	lv_obj_t *old = sm.screen;
	lv_group_t *old_group = sm.group;

	sm.group = lv_group_create();
	lv_group_set_wrap(sm.group, true);
	lv_group_set_default(sm.group);	/* focusable widgets join it on creation */
	sm.screen = widget_create(screen_ops[id], NULL);
	port_input_set_group(sm.group);
	lv_screen_load(sm.screen);
	/* the first widget is focused, show it as if the keypad moved there */
	if (lv_group_get_focused(sm.group))
		lv_obj_add_state(lv_group_get_focused(sm.group), LV_STATE_FOCUS_KEY);
	sm.shown = id;
	if (old)
		lv_obj_delete(old);
	if (old_group)
		lv_group_delete(old_group);
	LOGD("screens", "screen %d", id);
}

static void on_nav(void *self, const struct gui_ctx *c, uint32_t changed)
{
	enum screen_id id = ctx_screen(c);

	(void)self;
	(void)changed;
	if ((int)id != sm.shown && id < SCREEN_COUNT && screen_ops[id])
		show(id);
}

int screens_init(void)
{
	sm.shown = -1;
	sm.sub = view_subscribe(CTX_NAV, on_nav, NULL);
	return sm.sub > 0 ? 0 : -1;
}

void screens_deinit(void)
{
	view_unsubscribe(sm.sub);
	if (sm.screen) {
		lv_screen_load(lv_obj_create(NULL));	/* never delete the active screen */
		lv_obj_delete(sm.screen);
	}
	if (sm.group)
		lv_group_delete(sm.group);
	sm.screen = NULL;
	sm.group = NULL;
	sm.shown = -1;
}
