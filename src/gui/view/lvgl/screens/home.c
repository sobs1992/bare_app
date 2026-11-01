/*
 * Home: menu of screens. A minimised game shows a pause symbol; picking
 * another screen that needs the video layer (another game) first asks
 * whether to close it.
 */
#include <stdlib.h>

#include "../widget.h"
#include "gui/action.h"

#define BUTTON_H 40
#define BUTTON_GAP 8
#define DIALOG_W 400

struct entry {
	const char *symbol;
	enum str_id text;
	enum gui_action_type action;
	int arg;
	enum video_source video;	/* what the screen shows in the video layer */
};

static const struct entry entries[] = {
#ifdef CONFIG_GAME_HOMM3
	{ LV_SYMBOL_PLAY, S_HOMM3, ACT_OPEN_SCREEN, SCREEN_HOMM3, VIDEO_HOMM3 },
#endif
#ifdef CONFIG_GAME_DOOM
	{ LV_SYMBOL_PLAY, S_DOOM, ACT_OPEN_SCREEN, SCREEN_DOOM, VIDEO_DOOM },
#endif
#ifdef CONFIG_SCREEN_INPUT_TEST
	{ LV_SYMBOL_KEYBOARD, S_INPUT_TEST, ACT_OPEN_SCREEN, SCREEN_INPUT_TEST, VIDEO_NONE },
#endif
#ifdef CONFIG_SCREEN_SETTINGS
	{ LV_SYMBOL_SETTINGS, S_SETTINGS, ACT_OPEN_SCREEN, SCREEN_SETTINGS, VIDEO_NONE },
#endif
#ifdef CONFIG_SCREEN_LAYERS
	{ LV_SYMBOL_IMAGE, S_LAYERS, ACT_OPEN_SCREEN, SCREEN_LAYERS, VIDEO_TEST_PATTERN },
#endif
	{ LV_SYMBOL_POWER, S_QUIT, ACT_POWER_OFF, 0, VIDEO_NONE },
};

#define N_ENTRIES (sizeof(entries) / sizeof(entries[0]))

/* "Close the minimised game?" over the menu, with its own focus group */
struct dialog {
	lv_obj_t *backdrop;
	lv_group_t *group, *menu_group;
	const struct entry *picked;
};

static const struct entry *entry_of(enum video_source src)
{
	size_t i;

	for (i = 0; i < N_ENTRIES; i++)
		if (entries[i].video == src)
			return &entries[i];
	return NULL;
}

static void dialog_close(struct dialog *d)
{
	port_input_set_group(d->menu_group);
	/* from an event of its own button: deleted after the event */
	lv_obj_delete_async(d->backdrop);	/* on_dialog_delete frees the rest */
}

static void on_dialog_delete(lv_event_t *e)
{
	struct dialog *d = lv_event_get_user_data(e);

	/* also when the menu is rebuilt under it: its new group is set by then */
	lv_group_delete(d->group);
	free(d);
}

static void on_cancel(lv_event_t *e)
{
	dialog_close(lv_event_get_user_data(e));
}

static void on_close_and_start(lv_event_t *e)
{
	struct dialog *d = lv_event_get_user_data(e);
	const struct entry *en = d->picked;

	dialog_close(d);
	view_video_set(VIDEO_NONE);	/* ends the minimised game */
	gui_action_post(en->action, en->arg);
}

static lv_obj_t *dialog_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
			       struct dialog *d)
{
	lv_obj_t *btn = lv_button_create(parent), *label;

	lv_obj_set_size(btn, LV_PCT(100), BUTTON_H);
	ui_focus_style(btn);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, d);
	lv_obj_add_event_cb(btn, on_cancel, LV_EVENT_CANCEL, d);	/* B */
	label = lv_label_create(btn);
	lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
	lv_label_set_text(label, text);
	lv_obj_center(label);
	return btn;
}

static void ask_close(lv_obj_t *scr, const struct entry *picked, const struct entry *running)
{
	struct dialog *d = calloc(1, sizeof(*d));
	lv_obj_t *box, *label;

	if (!d)
		return;
	d->picked = picked;
	d->menu_group = lv_group_get_default();
	d->group = lv_group_create();
	lv_group_set_wrap(d->group, true);
	lv_group_set_default(d->group);	/* the buttons join it */

	d->backdrop = lv_obj_create(scr);
	lv_obj_remove_style_all(d->backdrop);
	lv_obj_set_floating(d->backdrop, true);
	lv_obj_set_size(d->backdrop, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(d->backdrop, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(d->backdrop, LV_OPA_60, 0);
	lv_obj_add_event_cb(d->backdrop, on_dialog_delete, LV_EVENT_DELETE, d);

	box = lv_obj_create(d->backdrop);
	lv_obj_set_size(box, DIALOG_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(box, BUTTON_GAP, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_center(box);
	label = lv_label_create(box);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text_fmt(label, tr(S_GAME_MINIMIZED_FMT), tr(running->text),
			      tr(picked->text));
	dialog_button(box, tr(S_CANCEL), on_cancel, d);
	dialog_button(box, tr(S_CLOSE_AND_START), on_close_and_start, d);

	lv_group_set_default(d->menu_group);
	port_input_set_group(d->group);
	lv_obj_add_state(lv_group_get_focused(d->group), LV_STATE_FOCUS_KEY);
}

static void on_click(lv_event_t *e)
{
	const struct entry *en = lv_event_get_user_data(e);
	enum video_source minimized = view_video_minimized();
	const struct entry *running = minimized != VIDEO_NONE ? entry_of(minimized) : NULL;

	if (running && en->video != VIDEO_NONE && en != running) {
		ask_close(lv_obj_get_screen(lv_event_get_target_obj(e)), en, running);
		return;
	}
	gui_action_post(en->action, en->arg);	/* the same game comes back */
}

static void *home_init(lv_obj_t *parent, lv_obj_t **root)
{
	lv_obj_t *scr, *body, *btn, *label;
	size_t i;

	(void)parent;
	scr = lv_obj_create(NULL);
	statusbar_create(scr, NULL, "");	/* battery only */
	body = screen_body(scr);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(body, BUTTON_GAP, 0);

	for (i = 0; i < N_ENTRIES; i++) {
		bool minimized = entries[i].video != VIDEO_NONE &&
				 entries[i].video == view_video_minimized();

		btn = lv_button_create(body);
		lv_obj_set_size(btn, 280, BUTTON_H);
		ui_focus_style(btn);
		lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, (void *)&entries[i]);
		label = lv_label_create(btn);
		lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
		lv_label_set_text_fmt(label, "%s  %s", minimized ? LV_SYMBOL_PAUSE : entries[i].symbol,
				      tr(entries[i].text));
		lv_obj_center(label);
	}

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
	return NULL;	/* no state */
}

const struct widget_ops home_screen = {
	.name = "home",
	.init = home_init,
};
