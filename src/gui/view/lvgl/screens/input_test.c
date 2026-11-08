/*
 * Input test: grabs the gamepad, lights up pressed buttons and shows the
 * sticks. Only what the hardware has is shown (input_*_present()). B is
 * one of the buttons under test, so it cannot simply mean "back": B
 * pressed twice within 600 ms leaves.
 *
 * A pressed twice calibrates the sticks (core/input, joystick.h): first
 * the rest position (the sticks held still for a second), then the edge
 * (the sticks turned along it; the boxes draw what was reached). A saves,
 * Y goes back to no calibration, B cancels. The test then shows the
 * calibrated sticks: the edge is the box's circle.
 */
#include <math.h>
#include <stdlib.h>

#include "../widget.h"
#include "core/joystick.h"
#include "core/util.h"
#include "gui/action.h"

#define STICK 96		/* stick box size */
#define DOT 14
#define EDGE_DOT 4
#define DOUBLE_TAP_MS 600
#define SAMPLE_MS 20
#define REST_MS 1000		/* held still that long: the rest position */
#define REST_SPREAD 0.04f	/* ...moving less than that */

#define LEFT_STICK  (GP_BIT(GP_AXIS_LX) | GP_BIT(GP_AXIS_LY))
#define RIGHT_STICK (GP_BIT(GP_AXIS_RX) | GP_BIT(GP_AXIS_RY))

enum mode { MODE_TEST, MODE_REST, MODE_TURN };

/* MODE_REST: the window a stick stayed in */
struct still {
	float x0, x1, y0, y1;
	uint64_t since;
};

struct input_test {
	lv_obj_t *cell[GP_COUNT];	/* NULL: the device has no such button */
	lv_obj_t *dot[INPUT_STICKS];	/* NULL: no such stick */
	lv_obj_t *edge[INPUT_STICKS][JOY_CAL_BINS];
	lv_obj_t *values, *hint;
	uint32_t last_a, last_b;
	enum mode mode;
	enum str_id note;		/* under the hint in MODE_TEST; S_COUNT: none */
	lv_timer_t *timer;
	struct still still[INPUT_STICKS];
	struct joy_cal_rec rec[INPUT_STICKS];
};

static void place(lv_obj_t *obj, int size, float x, float y)
{
	int half = (STICK - size) / 2;

	x = x < -1 ? -1 : x > 1 ? 1 : x;
	y = y < -1 ? -1 : y > 1 ? 1 : y;
	lv_obj_set_pos(obj, half + (int)lrintf(x * half), half + (int)lrintf(y * half));
}

/* The stick's dot: calibrated in the test, as the driver reports it while
 * calibrating (around the rest position found) */
static void place_dot(struct input_test *t, int stick)
{
	float x, y;

	if (!t->dot[stick])
		return;
	if (t->mode == MODE_TEST) {
		x = input_axis(stick ? GP_AXIS_RX : GP_AXIS_LX);
		y = input_axis(stick ? GP_AXIS_RY : GP_AXIS_LY);
	} else {
		input_raw_stick(stick, &x, &y);
		if (t->mode == MODE_TURN) {
			x -= t->rec[stick].cx;
			y -= t->rec[stick].cy;
		}
	}
	place(t->dot[stick], DOT, x, y);
}

static void show_edge(struct input_test *t, int stick)
{
	const struct joy_cal_rec *r = &t->rec[stick];
	int i;

	for (i = 0; i < JOY_CAL_BINS; i++) {
		lv_obj_t *e = t->edge[stick][i];

		if (!e)
			continue;
		if (t->mode != MODE_TURN || r->br[i] <= 0) {
			lv_obj_set_hidden(e, true);
			continue;
		}
		place(e, EDGE_DOT, r->bx[i], r->by[i]);
		lv_obj_set_hidden(e, false);
	}
}

static void show_values(struct input_test *t)
{
	float x, y;
	int s;

	if (!t->values)
		return;
	if (t->mode == MODE_TURN) {
		char text[64] = "";
		size_t n = 0;

		for (s = 0; s < INPUT_STICKS; s++)
			if (t->dot[s])
				n += (size_t)lv_snprintf(text + n, sizeof(text) - n, "%s%s %d/%d",
							 n ? "   " : "", s ? "R" : "L",
							 joy_cal_covered(&t->rec[s]), JOY_SECTORS);
		lv_label_set_text(t->values, text);
		return;
	}
	if (t->mode == MODE_REST) {
		lv_label_set_text(t->values, "");
		return;
	}
	if (t->dot[0] && t->dot[1]) {
		lv_label_set_text_fmt(t->values, "L %+.2f %+.2f\nR %+.2f %+.2f",
				      (double)input_axis(GP_AXIS_LX), (double)input_axis(GP_AXIS_LY),
				      (double)input_axis(GP_AXIS_RX), (double)input_axis(GP_AXIS_RY));
		return;
	}
	s = t->dot[0] ? 0 : 1;
	x = input_axis(s ? GP_AXIS_RX : GP_AXIS_LX);
	y = input_axis(s ? GP_AXIS_RY : GP_AXIS_LY);
	lv_label_set_text_fmt(t->values, "X %+.2f  Y %+.2f", (double)x, (double)y);
}

static void show_hint(struct input_test *t)
{
	if (t->mode == MODE_REST)
		lv_label_set_text(t->hint, tr(S_STICK_CAL_REST));
	else if (t->mode == MODE_TURN)
		lv_label_set_text(t->hint, tr(S_STICK_CAL_TURN));
	else if (!t->dot[0] && !t->dot[1])
		lv_label_set_text(t->hint, tr(S_TWICE_BACK));
	else if (t->note == S_COUNT)
		lv_label_set_text_fmt(t->hint, "%s\n%s", tr(S_TWICE_BACK), tr(S_STICK_CAL_START));
	else
		lv_label_set_text_fmt(t->hint, "%s\n%s\n%s", tr(S_TWICE_BACK), tr(S_STICK_CAL_START),
				      tr(t->note));
}

static void show(struct input_test *t)
{
	int s;

	for (s = 0; s < INPUT_STICKS; s++) {
		place_dot(t, s);
		show_edge(t, s);
	}
	show_values(t);
	show_hint(t);
}

static void set_mode(struct input_test *t, enum mode m)
{
	int s;

	t->mode = m;
	for (s = 0; s < INPUT_STICKS; s++)
		t->still[s].since = 0;
	if (m == MODE_TEST && t->timer) {
		lv_timer_delete(t->timer);
		t->timer = NULL;
	}
	show(t);
}

/* MODE_REST: done once every stick stayed still for REST_MS */
static bool rest_found(struct input_test *t)
{
	uint64_t now = now_ms();
	bool all = true;
	int s;

	for (s = 0; s < INPUT_STICKS; s++) {
		struct still *w = &t->still[s];
		float x, y;

		if (!t->dot[s])
			continue;
		input_raw_stick(s, &x, &y);
		if (!w->since || x < w->x1 - REST_SPREAD || x > w->x0 + REST_SPREAD ||
		    y < w->y1 - REST_SPREAD || y > w->y0 + REST_SPREAD) {
			*w = (struct still){ x, x, y, y, now };	/* moved: again from here */
		} else {
			w->x0 = fminf(w->x0, x);
			w->x1 = fmaxf(w->x1, x);
			w->y0 = fminf(w->y0, y);
			w->y1 = fmaxf(w->y1, y);
		}
		if (now - w->since < REST_MS)
			all = false;
	}
	if (!all)
		return false;
	for (s = 0; s < INPUT_STICKS; s++)
		joy_cal_begin(&t->rec[s], (t->still[s].x0 + t->still[s].x1) / 2,
			      (t->still[s].y0 + t->still[s].y1) / 2);
	return true;
}

static void on_sample(lv_timer_t *timer)
{
	struct input_test *t = lv_timer_get_user_data(timer);
	float x, y;
	int s;

	if (t->mode == MODE_REST) {
		if (rest_found(t))
			set_mode(t, MODE_TURN);
		return;
	}
	for (s = 0; s < INPUT_STICKS; s++)
		if (t->dot[s]) {
			input_raw_stick(s, &x, &y);
			joy_cal_add(&t->rec[s], x, y);
		}
	show(t);
}

/* A in MODE_TURN: every stick must have gone round its edge */
static void save(struct input_test *t, bool reset)
{
	struct joy_stick_cal cal[INPUT_STICKS];
	int s;

	if (!reset)
		for (s = 0; s < INPUT_STICKS; s++)
			if (t->dot[s] && !joy_cal_finish(&t->rec[s], &cal[s]))
				return;		/* keep turning */
	for (s = 0; s < INPUT_STICKS; s++)
		if (t->dot[s])
			input_set_calibration(s, reset ? NULL : &cal[s]);
	t->note = input_save_calibration() ? S_STICK_CAL_NOT_SAVED :
		  reset ? S_STICK_CAL_RESET : S_STICK_CAL_SAVED;
	set_mode(t, MODE_TEST);
}

static bool double_tap(uint32_t *last)
{
	uint32_t now = (uint32_t)now_ms();
	bool twice = *last && now - *last < DOUBLE_TAP_MS;

	*last = twice ? 0 : now;
	return twice;
}

static void on_calibrate_button(struct input_test *t, const struct gp_event *ev)
{
	if (!ev->pressed)
		return;
	if (ev->code == GP_B)
		set_mode(t, MODE_TEST);
	else if (ev->code == GP_A && t->mode == MODE_TURN)
		save(t, false);
	else if (ev->code == GP_Y && t->mode == MODE_TURN)
		save(t, true);
}

static void on_input(const struct gp_event *ev, void *ud)
{
	struct input_test *t = ud;

	if (ev->type == GP_EV_AXIS) {
		if (t->mode == MODE_TEST) {
			place_dot(t, ev->code == GP_AXIS_RX || ev->code == GP_AXIS_RY);
			show_values(t);
		}
		return;
	}
	if (ev->repeat)
		return;
	if (t->mode != MODE_TEST) {
		on_calibrate_button(t, ev);
		return;
	}
	if (!t->cell[ev->code])
		;	/* not declared by the driver (e.g. the stick's virtual D-pad) */
	else if (ev->pressed)
		lv_obj_add_state(t->cell[ev->code], LV_STATE_CHECKED);
	else
		lv_obj_remove_state(t->cell[ev->code], LV_STATE_CHECKED);
	if (!ev->pressed)
		return;
	if (ev->code == GP_B && double_tap(&t->last_b))
		gui_action_post(ACT_BACK, 0);
	if (ev->code == GP_A && double_tap(&t->last_a) && (t->dot[0] || t->dot[1])) {
		lv_obj_remove_state(t->cell[GP_A], LV_STATE_CHECKED);	/* its release goes unseen */
		t->note = S_COUNT;
		t->timer = lv_timer_create(on_sample, SAMPLE_MS, t);
		set_mode(t, MODE_REST);
	}
}

static lv_obj_t *stick_box(lv_obj_t *parent, const char *name, lv_obj_t **dot, lv_obj_t **edge)
{
	lv_obj_t *col = lv_obj_create(parent), *box, *label;
	int i;

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
	for (i = 0; i < JOY_CAL_BINS; i++) {
		edge[i] = lv_obj_create(box);
		lv_obj_remove_style_all(edge[i]);
		lv_obj_set_size(edge[i], EDGE_DOT, EDGE_DOT);
		lv_obj_set_style_radius(edge[i], EDGE_DOT / 2, 0);
		lv_obj_set_style_bg_opa(edge[i], LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(edge[i], UI_COLOR_DIM, 0);
		lv_obj_set_hidden(edge[i], true);
	}
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
	lv_obj_t *scr, *body, *left, *grid, *right, *sticks;
	uint32_t present, axes;
	int b;

	(void)parent;
	if (!t)
		return NULL;
	t->note = S_COUNT;
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
	present = input_buttons_present();
	if (!present)
		present = GP_BIT(GP_COUNT) - 1;	/* nothing declared: show all */
	for (b = 0; b < GP_COUNT; b++) {
		lv_obj_t *cell, *label;

		/* the power key never reaches a screen: it puts the device to sleep */
		if (!(present & GP_BIT(b)) || b == GP_POWER)
			continue;
		cell = lv_obj_create(grid);
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
	t->hint = lv_label_create(left);
	lv_obj_set_width(t->hint, LV_PCT(100));
	lv_label_set_long_mode(t->hint, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_color(t->hint, UI_COLOR_DIM, 0);

	/* right: both sticks and their values */
	right = column(body, 2 * STICK + 10);
	sticks = lv_obj_create(right);
	lv_obj_remove_style_all(sticks);
	lv_obj_set_size(sticks, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(sticks, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(sticks, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	axes = input_axes_present();
	if (axes & LEFT_STICK)
		stick_box(sticks, (axes & RIGHT_STICK) ? "L" : "", &t->dot[0], t->edge[0]);
	if (axes & RIGHT_STICK)
		stick_box(sticks, (axes & LEFT_STICK) ? "R" : "", &t->dot[1], t->edge[1]);
	if (axes & (LEFT_STICK | RIGHT_STICK)) {
		t->values = lv_label_create(right);
		lv_obj_set_style_text_color(t->values, UI_COLOR_DIM, 0);
	}

	show(t);
	view_input_grab(on_input, t);
	*root = scr;
	return t;
}

static void input_test_deinit(void *self)
{
	struct input_test *t = self;

	view_input_release(self);
	if (t->timer)
		lv_timer_delete(t->timer);
	free(self);
}

const struct widget_ops input_test_screen = {
	.name = "input-test",
	.deps = 0,
	.init = input_test_init,
	.update = NULL,
	.deinit = input_test_deinit,
};
