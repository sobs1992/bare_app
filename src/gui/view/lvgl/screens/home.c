/* Home: menu of screens, board name at the bottom (CTX_SYSTEM). */
#include <stdlib.h>

#include "../widget.h"
#include "gui/action.h"

#define BUTTON_H 40
#define BUTTON_GAP 8

struct home {
	lv_obj_t *footer;
};

struct entry {
	const char *symbol;
	enum str_id text;
	enum gui_action_type action;
	int arg;
};

static const struct entry entries[] = {
#ifdef CONFIG_GAME_DOOM
	{ LV_SYMBOL_PLAY, S_DOOM, ACT_OPEN_SCREEN, SCREEN_DOOM },
#endif
#ifdef CONFIG_SCREEN_INPUT_TEST
	{ LV_SYMBOL_KEYBOARD, S_INPUT_TEST, ACT_OPEN_SCREEN, SCREEN_INPUT_TEST },
#endif
#ifdef CONFIG_SCREEN_SETTINGS
	{ LV_SYMBOL_SETTINGS, S_SETTINGS, ACT_OPEN_SCREEN, SCREEN_SETTINGS },
#endif
#ifdef CONFIG_SCREEN_LAYERS
	{ LV_SYMBOL_IMAGE, S_LAYERS, ACT_OPEN_SCREEN, SCREEN_LAYERS },
#endif
	{ LV_SYMBOL_POWER, S_QUIT, ACT_QUIT, 0 },
};

static void on_click(lv_event_t *e)
{
	const struct entry *en = lv_event_get_user_data(e);

	gui_action_post(en->action, en->arg);
}

static void *home_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct home *h = calloc(1, sizeof(*h));
	lv_obj_t *scr, *body, *btn, *label;
	size_t i;

	(void)parent;
	if (!h)
		return NULL;
	scr = lv_obj_create(NULL);
	statusbar_create(scr, NULL, "bare_app");
	body = screen_body(scr);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(body, BUTTON_GAP, 0);

	for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		btn = lv_button_create(body);
		lv_obj_set_size(btn, 280, BUTTON_H);
		ui_focus_style(btn);
		lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, (void *)&entries[i]);
		label = lv_label_create(btn);
		lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
		lv_label_set_text_fmt(label, "%s  %s", entries[i].symbol, tr(entries[i].text));
		lv_obj_center(label);
	}

	h->footer = lv_label_create(body);
	lv_obj_set_style_text_color(h->footer, UI_COLOR_DIM, 0);
	lv_label_set_text(h->footer, "");
	/*
	 * Centred content that does not fit sticks out above the body too,
	 * under the status bar, where scrolling cannot reach it: then start
	 * from the top and let the focus scroll the list.
	 */
	lv_obj_update_layout(scr);
	if (lv_obj_get_scroll_top(body) > 0 || lv_obj_get_scroll_bottom(body) > 0) {
		lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_scroll_to_y(body, 0, LV_ANIM_OFF);
	}
	*root = scr;
	return h;
}

static void home_update(void *self, const struct gui_ctx *c, uint32_t changed)
{
	struct home *h = self;

	if (changed & CTX_SYSTEM)
		lv_label_set_text_fmt(h->footer, tr(S_PLATFORM_FMT),
				      *c->system.model ? c->system.model : tr(S_UNKNOWN_BOARD),
				      c->system.platform);
}

static void home_deinit(void *self)
{
	free(self);
}

const struct widget_ops home_screen = {
	.name = "home",
	.deps = CTX_SYSTEM,
	.init = home_init,
	.update = home_update,
	.deinit = home_deinit,
};
