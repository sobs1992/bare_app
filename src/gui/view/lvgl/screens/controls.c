/*
 * A game's own controls (SCREEN_CONTROLS, for the game of
 * view_game_selected(); X on its tile in the home menu opens it): a row
 * per button of the device the game gets, with the button it is for the
 * game. Left and Right (or A) go through the buttons and "nothing", X sets
 * the row back; the last row sets them all back. Every change is an
 * ACT_SET_CONTROL at once (ctx settings.controls, kept in the config) and
 * counts from the next press, also in a minimised game. Under the buttons
 * a row per axis of each stick the device has: the game gets it inverted
 * or not (A, Left or Right turn it over, X back to not; ACT_SET_INVERT).
 * The game's own hint at the top says what its buttons do.
 *
 * Not in the list: what bare_app keeps before the game sees it (the power
 * and volume keys, the quit combo, the game's minimise button, L2 and R2
 * of a game that leaves them for the volume).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../widget.h"
#include "games/game.h"
#include "gui/action.h"

#define NAME_CELLS 4		/* the device's button */

struct controls {
	struct game_ref game;
	char key[GAME_KEY_SIZE];
	lv_obj_t *list, *footer, *reset;
	lv_obj_t *rows[GP_COUNT], *values[GP_COUNT];
	int8_t shown[GP_COUNT];		/* the button shown in each row, -2 nothing yet */
	lv_obj_t *axis_rows[GP_AXIS_COUNT], *axis_values[GP_AXIS_COUNT];
	int8_t axis_shown[GP_AXIS_COUNT];	/* inverted shown: 1, 0, -1 nothing yet */
	struct gp_remap map;		/* the game's, as the context says */
	lv_group_t *group;		/* the screen's */
};

/* What a button can be for the game, in the order Left and Right go */
static bool target_ok(int b)
{
	return b == GP_NONE || (b >= 0 && b < GP_COUNT && b != GP_VOL_UP && b != GP_VOL_DOWN &&
				b != GP_POWER);
}

static int next_target(int b, int dir)
{
	/* GP_NONE (-1) after the last button and before the first */
	do {
		b += dir;
		if (b >= GP_COUNT)
			b = GP_NONE;
		else if (b < GP_NONE)
			b = GP_COUNT - 1;
	} while (!target_ok(b));
	return b;
}

static int row_axis(struct controls *c, lv_obj_t *row)
{
	int a;

	for (a = 0; a < GP_AXIS_COUNT; a++)
		if (c->axis_rows[a] == row)
			return a;
	return -1;
}

static int row_button(struct controls *c, lv_obj_t *row)
{
	int b;

	for (b = 0; b < GP_COUNT; b++)
		if (c->rows[b] == row)
			return b;
	return -1;
}

static void hints(struct controls *c, bool reset_row)
{
	if (reset_row)
		footer_set(c->footer, "B", tr(S_BACK), "A", tr(S_RESET), NULL);
	else
		footer_set(c->footer, "X", tr(S_DEFAULT), "B", tr(S_BACK),
			   LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, tr(S_CHANGE), NULL);
}

static void on_focused(lv_event_t *e)
{
	struct controls *c = lv_event_get_user_data(e);
	lv_obj_t *obj = lv_event_get_target_obj(e);

	lv_obj_scroll_to_view(obj, LV_ANIM_ON);
	hints(c, obj == c->reset);
}

static void set(struct controls *c, int b, int to)
{
	gui_action_post_str_arg(ACT_SET_CONTROL, c->key, ACT_CONTROL_ARG(b, to));
}

static void set_invert(struct controls *c, int a, bool on)
{
	gui_action_post_str_arg(ACT_SET_INVERT, c->key, ACT_INVERT_ARG(a, on));
}

static bool inverted(const struct controls *c, int a)
{
	return c->map.invert & GP_BIT(a);
}

static void on_key(lv_event_t *e)
{
	struct controls *c = lv_event_get_user_data(e);
	lv_obj_t *obj = lv_event_get_target_obj(e);
	int b = row_button(c, obj), a = row_axis(c, obj);
	uint32_t key = lv_event_get_key(e);

	if (a >= 0) {
		if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT)
			set_invert(c, a, !inverted(c, a));
		else if (key == UI_LV_KEY_ALT)
			set_invert(c, a, false);
		return;
	}
	if (b < 0)
		return;
	if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT)
		set(c, b, next_target(c->map.to[b], key == LV_KEY_RIGHT ? 1 : -1));
	else if (key == UI_LV_KEY_ALT)
		set(c, b, b);
}

static void on_click(lv_event_t *e)
{
	struct controls *c = lv_event_get_user_data(e);
	lv_obj_t *obj = lv_event_get_target_obj(e);
	int b = row_button(c, obj), a = row_axis(c, obj);

	if (obj == c->reset)
		gui_action_post_str(ACT_RESET_CONTROLS, c->key);
	else if (a >= 0)
		set_invert(c, a, !inverted(c, a));
	else if (b >= 0)
		set(c, b, next_target(c->map.to[b], 1));
}

/* What the game gets of the device */
static uint32_t buttons_listed(struct game_ref g)
{
	const struct platform *pf = view_platform();
	uint32_t present = input_buttons_present();
	enum gp_button minimize = game_minimize_button(g, pf);

	if (!present)
		present = GP_BIT(GP_COUNT) - 1;	/* nothing declared: all */
	present &= ~(GP_BIT(GP_POWER) | GP_BIT(GP_VOL_UP) | GP_BIT(GP_VOL_DOWN));
	if (pf)
		present &= ~pf->quit_combo;
	if (minimize >= 0 && minimize < GP_COUNT)
		present &= ~GP_BIT(minimize);
	if (game_trigger_volume(g))
		present &= ~(GP_BIT(GP_L2) | GP_BIT(GP_R2));
	return present;
}

/* The sticks' axes of the device (none declared: all) */
static uint32_t axes_listed(void)
{
	uint32_t present = input_axes_present();

	return present ? present : GP_BIT(GP_AXIS_COUNT) - 1;
}

static lv_obj_t *row_new(struct controls *c, lv_group_t *group)
{
	lv_obj_t *r = console_row(c->list);

	lv_group_add_obj(group, r);
	lv_obj_add_event_cb(r, on_focused, LV_EVENT_FOCUSED, c);
	lv_obj_add_event_cb(r, on_click, LV_EVENT_CLICKED, c);
	return r;
}

static void *controls_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct controls *c = calloc(1, sizeof(*c));
	lv_group_t *group = lv_group_get_default();
	char title[160];
	lv_obj_t *scr, *label;
	uint32_t listed;
	int b;

	(void)parent;
	if (!c)
		return NULL;
	c->group = group;
	c->game = view_game_selected();
	game_key(c->game, c->key);
	gp_remap_reset(&c->map);
	memset(c->shown, -2, sizeof(c->shown));
	memset(c->axis_shown, -1, sizeof(c->axis_shown));
	snprintf(title, sizeof(title), "%s: %s", tr(S_CONTROLS),
		 c->game.def ? game_title(c->game) : "");
	scr = console_screen(LV_SYMBOL_SHUFFLE, title);
	c->footer = footer_create(scr);
	c->list = screen_body(scr);
	lv_obj_set_style_pad_row(c->list, 0, 0);
	lv_obj_set_scroll_dir(c->list, LV_DIR_VER);
	*root = scr;
	if (!c->game.def)
		return c;

	/* what the game's buttons do */
	label = lv_label_create(c->list);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_color(label, UI_COLOR_DIM, 0);
	lv_obj_set_style_pad_bottom(label, ui_cell() / 2, 0);
	lv_label_set_text(label, game_hint(c->game, view_platform()));

	listed = buttons_listed(c->game);
	for (b = 0; b < GP_COUNT; b++) {
		lv_obj_t *r;

		if (!(listed & GP_BIT(b)))
			continue;
		r = c->rows[b] = row_new(c, group);
		lv_obj_add_event_cb(r, on_key, LV_EVENT_KEY, c);
		label = lv_label_create(r);
		lv_obj_set_width(label, NAME_CELLS * ui_cell());
		lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
		lv_label_set_text(label, ui_button_name(b));
		label = lv_label_create(r);
		lv_obj_set_style_text_color(label, UI_COLOR_DIM, 0);
		lv_label_set_text(label, "\xc2\xbb");	/* » */
		c->values[b] = lv_label_create(r);
		lv_obj_set_style_pad_left(c->values[b], ui_cell(), 0);
		lv_obj_set_style_text_font(c->values[b], UI_FONT_MID, 0);
	}
	listed = axes_listed();
	for (b = 0; b < GP_AXIS_COUNT; b++) {
		lv_obj_t *r;

		if (!(listed & GP_BIT(b)))
			continue;
		r = c->axis_rows[b] = row_new(c, group);
		lv_obj_add_event_cb(r, on_key, LV_EVENT_KEY, c);
		label = lv_label_create(r);
		lv_obj_set_flex_grow(label, 1);
		lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
		lv_label_set_text_fmt(label, "%s: %s", tr(b < GP_AXIS_RX ? S_LEFT_STICK : S_RIGHT_STICK),
				      tr(b == GP_AXIS_LX || b == GP_AXIS_RX ? S_INVERT_X : S_INVERT_Y));
		c->axis_values[b] = lv_label_create(r);
		lv_obj_set_style_text_font(c->axis_values[b], UI_FONT_MID, 0);
	}
	c->reset = row_new(c, group);
	label = lv_label_create(c->reset);
	lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
	lv_label_set_text(label, tr(S_CONTROLS_RESET_ALL));
	hints(c, false);
	return c;
}

static void controls_update(void *self, const struct gui_ctx *ctx, uint32_t changed)
{
	struct controls *c = self;
	const struct gp_remap *m = ctx_controls(ctx, c->key);
	int b;

	(void)changed;
	if (m)
		c->map = *m;
	else
		gp_remap_reset(&c->map);
	for (b = 0; b < GP_COUNT; b++) {
		int to = c->map.to[b];

		if (!c->values[b] || c->shown[b] == to)
			continue;
		c->shown[b] = (int8_t)to;
		lv_label_set_text(c->values[b], ui_button_name(to));
		/* changed: in the accent colour */
		lv_obj_set_style_text_color(c->values[b], to == b ? lv_color_white() : UI_COLOR_ACCENT, 0);
	}
	for (b = 0; b < GP_AXIS_COUNT; b++) {
		int on = inverted(c, b);

		if (!c->axis_values[b] || c->axis_shown[b] == on)
			continue;
		c->axis_shown[b] = (int8_t)on;
		lv_label_set_text(c->axis_values[b], tr(on ? S_ON : S_OFF));
		lv_obj_set_style_text_color(c->axis_values[b], on ? UI_COLOR_ACCENT : lv_color_white(), 0);
	}
}

static void controls_deinit(void *self)
{
	struct controls *c = self;

	/* LVGL deletes the rows after this: none may take the focus and call us */
	lv_group_remove_all_objs(c->group);
	free(c);
}

const struct widget_ops controls_screen = {
	.name = "controls",
	.deps = CTX_SETTINGS,
	.init = controls_init,
	.update = controls_update,
	.deinit = controls_deinit,
};
