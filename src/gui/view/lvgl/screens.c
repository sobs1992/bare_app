/*
 * Screen manager: follows CTX_NAV. Each screen gets its own focus group,
 * so the focus never lands on objects of the screen being replaced.
 */
#include <stdio.h>
#include <string.h>

#include "lvgl_view.h"

#include "core/log.h"
#include "widget.h"

static const struct widget_ops *const screen_ops[SCREEN_COUNT] = {
	[SCREEN_HOME] = &home_screen,
#ifdef CONFIG_SCREEN_INPUT_TEST
	[SCREEN_INPUT_TEST] = &input_test_screen,
#endif
#ifdef CONFIG_SCREEN_SETTINGS
	[SCREEN_SETTINGS] = &settings_screen,
#endif
#ifdef CONFIG_SCREEN_LAYERS
	[SCREEN_LAYERS] = &layers_screen,
#endif
#ifdef CONFIG_GAME_DOOM
	[SCREEN_DOOM] = &doom_screen,
#endif
#ifdef CONFIG_SCREEN_WIFI
	[SCREEN_WIFI] = &wifi_screen,
	[SCREEN_WIFI_KEY] = &wifi_key_screen,
#endif
};

static struct {
	int sub;
	int shown;		/* enum screen_id, -1 none */
	lv_obj_t *screen;
	lv_group_t *group;
	char lang[16];		/* language the screen was built in */
} sm = { .shown = -1 };

static int focused_index(lv_group_t *g)
{
	lv_obj_t *f = g ? lv_group_get_focused(g) : NULL;
	uint32_t i;

	for (i = 0; f && i < lv_group_get_obj_count(g); i++)
		if (lv_group_get_obj_by_index(g, i) == f)
			return (int)i;
	return 0;
}

/* keep_focus: rebuilding the same screen (new language), focus the same widget */
static void show(enum screen_id id, bool keep_focus)
{
	lv_obj_t *old = sm.screen;
	lv_group_t *old_group = sm.group;
	int focus = keep_focus ? focused_index(old_group) : 0;

	sm.group = lv_group_create();
	lv_group_set_wrap(sm.group, true);
	lv_group_set_default(sm.group);	/* focusable widgets join it on creation */
	sm.screen = widget_create(screen_ops[id], NULL);
	port_input_set_group(sm.group);
	if (focus > 0 && (uint32_t)focus < lv_group_get_obj_count(sm.group))
		lv_group_focus_obj(lv_group_get_obj_by_index(sm.group, focus));
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

static void on_ctx(void *self, const struct gui_ctx *c, uint32_t changed)
{
	enum screen_id id = ctx_screen(c);
	bool relang = (changed & CTX_SETTINGS) && strcmp(c->settings.lang, sm.lang);

	(void)self;
	if (relang) {
		if (i18n_set(c->settings.lang) < 0)
			LOGW("screens", "unknown language \"%s\", keeping %s", c->settings.lang, i18n_lang());
		snprintf(sm.lang, sizeof(sm.lang), "%s", c->settings.lang);
	}
	if (id >= SCREEN_COUNT || !screen_ops[id])
		return;
	if ((int)id != sm.shown)
		show(id, false);
	else if (relang)
		show(id, true);		/* every text comes back in the new language */
}

int screens_init(void)
{
	sm.shown = -1;
	sm.lang[0] = '\0';
	sm.sub = view_subscribe(CTX_NAV | CTX_SETTINGS, on_ctx, NULL);
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
