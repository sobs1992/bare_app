/*
 * Input test: grabs the gamepad, lights up pressed buttons and shows the
 * sticks. Only what the hardware has is shown (input_*_present()), laid
 * out as on a gamepad over the body of the screen (in cells of the grid,
 * widgets/console.c): the shoulder buttons along the top, the D-pad and
 * the left stick under it on the left, X Y A B and the right stick on
 * the right, the other buttons and the stick values in the middle. B is
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

#define PAD_W 22		/* the gamepad's width in cells; 11 high (the body) */
#define DOT 14			/* of the 480x320 layout (ui_px()) */
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
	int32_t stick, dot_d, edge_d;	/* pixels: a stick's box, its dot, an edge dot */
	lv_obj_t *cell[GP_COUNT];	/* NULL: the device has no such button */
	lv_obj_t *dot[INPUT_STICKS];	/* NULL: no such stick */
	lv_obj_t *edge[INPUT_STICKS][JOY_CAL_BINS];
	lv_obj_t *values, *hint, *footer;
	uint32_t last_a, last_b;
	enum mode mode;
	enum str_id note;		/* under the hint in MODE_TEST; S_COUNT: none */
	lv_timer_t *timer;
	struct still still[INPUT_STICKS];
	struct joy_cal_rec rec[INPUT_STICKS];
};

static void place(const struct input_test *t, lv_obj_t *obj, int size, float x, float y)
{
	int half = (t->stick - size) / 2;

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
	place(t, t->dot[stick], t->dot_d, x, y);
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
		place(t, e, t->edge_d, r->bx[i], r->by[i]);
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
	else
		lv_label_set_text(t->hint, t->note == S_COUNT ? "" : tr(t->note));
	if (t->mode != MODE_TEST)
		footer_set(t->footer, "B", tr(S_CANCEL), NULL);
	else if (t->dot[0] || t->dot[1])
		footer_set(t->footer, "B\xc3\x97" "2", tr(S_BACK), "A\xc3\x97" "2", tr(S_CALIBRATE), NULL);
	else
		footer_set(t->footer, "B\xc3\x97" "2", tr(S_BACK), NULL);
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

/* A stick: a round box at (x, y) cells of the pad, its edge dots, its dot */
static void stick_box(struct input_test *t, lv_obj_t *pad, int32_t x, int32_t y, lv_obj_t **dot,
		      lv_obj_t **edge)
{
	lv_obj_t *box = lv_obj_create(pad);
	int i;

	lv_obj_set_size(box, t->stick, t->stick);
	lv_obj_set_pos(box, x, y);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_style_radius(box, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(box, lv_color_hex(0x232833), 0);
	lv_obj_set_style_border_color(box, UI_COLOR_LINE, 0);
	lv_obj_set_scrollable(box, false);
	for (i = 0; i < JOY_CAL_BINS; i++) {
		edge[i] = lv_obj_create(box);
		lv_obj_remove_style_all(edge[i]);
		lv_obj_set_size(edge[i], t->edge_d, t->edge_d);
		lv_obj_set_style_radius(edge[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(edge[i], LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(edge[i], UI_COLOR_DIM, 0);
		lv_obj_set_hidden(edge[i], true);
	}
	*dot = lv_obj_create(box);
	lv_obj_remove_style_all(*dot);
	lv_obj_set_size(*dot, t->dot_d, t->dot_d);
	lv_obj_set_style_radius(*dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(*dot, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(*dot, UI_COLOR_ON, 0);
}

/* Where each button is on the pad: x, y, width, height in quarter cells */
static const struct {
	int8_t x, y, w, h;
	bool round;
} spots[GP_COUNT] = {
	/* the D-pad around 3.75 cells, X Y A B around 18.25: 1.75 cell buttons */
	[GP_UP]    = { 12, 8, 7, 7, false },  [GP_DOWN]  = { 12, 22, 7, 7, false },
	[GP_LEFT]  = { 5, 15, 7, 7, false },  [GP_RIGHT] = { 19, 15, 7, 7, false },
	[GP_X]     = { 70, 8, 7, 7, true },   [GP_B]     = { 70, 22, 7, 7, true },
	[GP_Y]     = { 63, 15, 7, 7, true },  [GP_A]     = { 77, 15, 7, 7, true },
	/* the shoulders */
	[GP_L2]    = { 0, 0, 10, 6, false },  [GP_L1]    = { 12, 0, 10, 6, false },
	[GP_R1]    = { 66, 0, 10, 6, false }, [GP_R2]    = { 78, 0, 10, 6, false },
	/* the middle */
	[GP_VOL_DOWN] = { 31, 0, 8, 6, false }, [GP_MENU] = { 40, 0, 8, 6, false },
	[GP_VOL_UP] = { 49, 0, 8, 6, false },
	[GP_SELECT] = { 32, 8, 11, 6, false }, [GP_START] = { 45, 8, 11, 6, false },
	[GP_F1]    = { 31, 16, 8, 6, false }, [GP_F2]    = { 40, 16, 8, 6, false },
	[GP_F3]    = { 49, 16, 8, 6, false }, [GP_F4]    = { 31, 24, 8, 6, false },
	[GP_F5]    = { 40, 24, 8, 6, false }, [GP_F6]    = { 49, 24, 8, 6, false },
};

static const char *button_text(enum gp_button b)
{
	switch (b) {
	case GP_UP:       return LV_SYMBOL_UP;
	case GP_DOWN:     return LV_SYMBOL_DOWN;
	case GP_LEFT:     return LV_SYMBOL_LEFT;
	case GP_RIGHT:    return LV_SYMBOL_RIGHT;
	case GP_A:        return "A";
	case GP_B:        return "B";
	case GP_X:        return "X";
	case GP_Y:        return "Y";
	case GP_L1:       return "L1";
	case GP_L2:       return "L2";
	case GP_R1:       return "R1";
	case GP_R2:       return "R2";
	case GP_SELECT:   return "Select";
	case GP_START:    return "Start";
	case GP_MENU:     return "Menu";
	case GP_VOL_UP:   return "Vol+";
	case GP_VOL_DOWN: return "Vol-";
	case GP_F1:       return "F1";
	case GP_F2:       return "F2";
	case GP_F3:       return "F3";
	case GP_F4:       return "F4";
	case GP_F5:       return "F5";
	case GP_F6:       return "F6";
	default:          return gp_button_name(b);
	}
}

static lv_obj_t *button_new(lv_obj_t *pad, enum gp_button b, int32_t q)
{
	lv_obj_t *cell = lv_obj_create(pad), *label;
	bool big = spots[b].w >= 7 && spots[b].h >= 7;

	lv_obj_set_pos(cell, spots[b].x * q, spots[b].y * q);
	lv_obj_set_size(cell, spots[b].w * q, spots[b].h * q);
	lv_obj_set_style_pad_all(cell, 0, 0);
	lv_obj_set_style_radius(cell, spots[b].round ? LV_RADIUS_CIRCLE : ui_px(8), 0);
	lv_obj_set_style_bg_color(cell, lv_color_hex(0x2a2f39), 0);
	lv_obj_set_style_border_width(cell, 0, 0);
	lv_obj_set_style_bg_color(cell, UI_COLOR_ON, LV_STATE_CHECKED);
	lv_obj_set_scrollable(cell, false);
	label = lv_label_create(cell);
	lv_obj_set_style_text_font(label, big ? UI_FONT_MID : UI_FONT_SMALL, 0);
	lv_label_set_text(label, button_text(b));
	lv_obj_center(label);
	return cell;
}

static void *input_test_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct input_test *t = calloc(1, sizeof(*t));
	int32_t u = ui_cell(), q = u / 4, stick_y = 30 * q;
	lv_obj_t *scr, *pad, *text;
	uint32_t present, axes;
	lv_area_t body;
	int b;

	(void)parent;
	if (!t)
		return NULL;
	t->note = S_COUNT;
	t->stick = 14 * q;		/* 3.5 cells */
	t->dot_d = ui_px(DOT);
	t->edge_d = LV_MAX(2, ui_px(EDGE_DOT));
	scr = console_screen(LV_SYMBOL_KEYBOARD, tr(S_INPUT_TEST));
	t->footer = footer_create(scr);

	/* the pad PAD_W cells wide in the middle of the body */
	ui_body_area(&body);
	pad = lv_obj_create(scr);
	lv_obj_remove_style_all(pad);
	lv_obj_set_size(pad, PAD_W * u, lv_area_get_height(&body));
	lv_obj_set_pos(pad, body.x1 + (lv_area_get_width(&body) - PAD_W * u) / 2, body.y1);
	lv_obj_set_scrollable(pad, false);

	present = input_buttons_present();
	if (!present)
		present = GP_BIT(GP_COUNT) - 1;	/* nothing declared: show all */
	for (b = 0; b < GP_COUNT; b++) {
		/* the power key never reaches a screen: it puts the device to sleep */
		if (!(present & GP_BIT(b)) || b == GP_POWER || !spots[b].w)
			continue;
		t->cell[b] = button_new(pad, b, q);
		if (input_buttons() & GP_BIT(b))
			lv_obj_add_state(t->cell[b], LV_STATE_CHECKED);
	}

	/* the sticks under the D-pad and X Y A B */
	axes = input_axes_present();
	if (axes & LEFT_STICK)
		stick_box(t, pad, 15 * q - t->stick / 2, stick_y, &t->dot[0], t->edge[0]);
	if (axes & RIGHT_STICK)
		stick_box(t, pad, 73 * q - t->stick / 2, stick_y, &t->dot[1], t->edge[1]);

	/* the values and what to do, in the middle under the buttons */
	text = lv_obj_create(pad);
	lv_obj_remove_style_all(text);
	lv_obj_set_pos(text, 29 * q, 32 * q);
	lv_obj_set_size(text, 30 * q, lv_area_get_height(&body) - 32 * q);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(text, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	if (axes & (LEFT_STICK | RIGHT_STICK)) {
		t->values = lv_label_create(text);
		lv_obj_set_style_text_align(t->values, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_color(t->values, UI_COLOR_DIM, 0);
	}
	t->hint = lv_label_create(text);
	lv_obj_set_width(t->hint, LV_PCT(100));
	lv_label_set_long_mode(t->hint, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(t->hint, LV_TEXT_ALIGN_CENTER, 0);

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
