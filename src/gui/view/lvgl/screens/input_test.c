/*
 * Input test: grabs the gamepad, lights up pressed buttons and shows the
 * sticks. Every button is shown, so B cannot simply mean "back": B pressed
 * twice within 600 ms leaves.
 */
#include <stdlib.h>

#include "../widget.h"
#include "core/util.h"
#include "gui/action.h"

#define STICK 96		/* stick box size */
#define DOT 14
#define DOUBLE_TAP_MS 600

struct input_test {
	lv_obj_t *cell[GP_COUNT];
	lv_obj_t *dot[2];
	lv_obj_t *values;
	uint32_t last_b;
};

static void place_dot(struct input_test *t, int stick)
{
	float x = input_axis(stick ? GP_AXIS_RX : GP_AXIS_LX);
	float y = input_axis(stick ? GP_AXIS_RY : GP_AXIS_LY);
	int half = (STICK - DOT) / 2;

	lv_obj_set_pos(t->dot[stick], half + (int)(x * half), half + (int)(y * half));
}

static void show_values(struct input_test *t)
{
	lv_label_set_text_fmt(t->values, "L %+.2f %+.2f\nR %+.2f %+.2f",
			      (double)input_axis(GP_AXIS_LX), (double)input_axis(GP_AXIS_LY),
			      (double)input_axis(GP_AXIS_RX), (double)input_axis(GP_AXIS_RY));
}

static void on_input(const struct gp_event *ev, void *ud)
{
	struct input_test *t = ud;

	if (ev->type == GP_EV_AXIS) {
		place_dot(t, ev->code == GP_AXIS_RX || ev->code == GP_AXIS_RY);
		show_values(t);
		return;
	}
	if (ev->repeat)
		return;
	if (ev->pressed)
		lv_obj_add_state(t->cell[ev->code], LV_STATE_CHECKED);
	else
		lv_obj_remove_state(t->cell[ev->code], LV_STATE_CHECKED);
	if (ev->code == GP_B && ev->pressed) {
		uint32_t now = (uint32_t)now_ms();

		if (t->last_b && now - t->last_b < DOUBLE_TAP_MS)
			gui_action_post(ACT_BACK, 0);
		t->last_b = now;
	}
}

static lv_obj_t *stick_box(lv_obj_t *parent, const char *name, lv_obj_t **dot)
{
	lv_obj_t *col = lv_obj_create(parent), *box, *label;

	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(col, 4, 0);
	label = lv_label_create(col);
	lv_label_set_text(label, name);
	box = lv_obj_create(col);
	lv_obj_set_size(box, STICK, STICK);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_style_radius(box, STICK / 2, 0);
	lv_obj_set_scrollable(box, false);
	*dot = lv_obj_create(box);
	lv_obj_set_size(*dot, DOT, DOT);
	lv_obj_set_style_radius(*dot, DOT / 2, 0);
	lv_obj_set_style_bg_color(*dot, UI_COLOR_ON, 0);
	lv_obj_set_style_border_width(*dot, 0, 0);
	return col;
}

static lv_obj_t *column(lv_obj_t *parent, int width)
{
	lv_obj_t *c = lv_obj_create(parent);

	lv_obj_remove_style_all(c);
	lv_obj_set_size(c, width, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(c, 8, 0);
	return c;
}

static void *input_test_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct input_test *t = calloc(1, sizeof(*t));
	lv_obj_t *scr, *body, *left, *grid, *right, *sticks, *hint;
	int b;

	(void)parent;
	if (!t)
		return NULL;
	scr = lv_obj_create(NULL);
	statusbar_create(scr, LV_SYMBOL_KEYBOARD, tr(S_INPUT_TEST));
	body = screen_body(scr);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	/* left: one cell per logical button, 5 per row */
	left = column(body, 250);
	grid = lv_obj_create(left);
	lv_obj_remove_style_all(grid);
	lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_gap(grid, 5, 0);
	for (b = 0; b < GP_COUNT; b++) {
		lv_obj_t *cell = lv_obj_create(grid), *label;

		lv_obj_set_size(cell, 46, 30);
		lv_obj_set_style_pad_all(cell, 0, 0);
		lv_obj_set_scrollable(cell, false);
		lv_obj_set_style_bg_color(cell, UI_COLOR_ON, LV_STATE_CHECKED);
		label = lv_label_create(cell);
		lv_label_set_text(label, gp_button_name(b));
		lv_obj_center(label);
		if (input_buttons() & GP_BIT(b))
			lv_obj_add_state(cell, LV_STATE_CHECKED);
		t->cell[b] = cell;
	}
	hint = lv_label_create(left);
	lv_obj_set_style_text_color(hint, UI_COLOR_DIM, 0);
	lv_label_set_text(hint, tr(S_TWICE_BACK));

	/* right: both sticks and their values */
	right = column(body, 2 * STICK + 10);
	sticks = lv_obj_create(right);
	lv_obj_remove_style_all(sticks);
	lv_obj_set_size(sticks, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(sticks, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(sticks, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	stick_box(sticks, "L", &t->dot[0]);
	stick_box(sticks, "R", &t->dot[1]);
	t->values = lv_label_create(right);
	lv_obj_set_style_text_color(t->values, UI_COLOR_DIM, 0);

	place_dot(t, 0);
	place_dot(t, 1);
	show_values(t);
	view_input_grab(on_input, t);
	*root = scr;
	return t;
}

static void input_test_deinit(void *self)
{
	view_input_release(self);
	free(self);
}

const struct widget_ops input_test_screen = {
	.name = "input-test",
	.deps = 0,
	.init = input_test_init,
	.update = NULL,
	.deinit = input_test_deinit,
};
