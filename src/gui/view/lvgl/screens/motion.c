/*
 * Motion sensor (SCREEN_MOTION, Settings > Games): whether the sensor is
 * plugged in and how fast it samples, whether the gyro's zero and up and
 * down are known, and a cross in a box that follows the device's turns
 * (GP_EV_MOTION) as a game's view would, MOTION_BOX_DEG across.
 *
 * A calibrates (core/motion.h): the device lies still for CAL_STILL_MS
 * (the gyro's zero), then is picked up and held as in a game; once held
 * steady its top is tilted towards the player by CAL_TILT_DEG, which gives
 * the device's right in the sensor's axes (up and down). The result goes
 * to core/input and is saved with the sticks' calibration. X puts the
 * cross back in the middle, Y removes the calibration, B leaves (or stops
 * a calibration).
 *
 * Over the state, the sensor's tune for every game (core/input
 * input_motion_tune): the sensitivity and each axis inverted or not. Up
 * and Down choose a row, Left and Right change it at once (the cross shows
 * it); it is saved with the calibration when the screen closes.
 */
#include <math.h>
#include <stdlib.h>

#include "../widget.h"
#include "core/motion.h"
#include "gui/action.h"

#define BOX_CELLS 9		/* the cross's box, square, at most the body's height */
#define CROSS_PX 18		/* of the 480x320 layout (ui_px()) */
#define MOTION_BOX_DEG 40.0f	/* the box from edge to edge */
#define STATUS_MS 250
#define CAL_STILL_MS 1500
#define CAL_PICKED_DPS 25.0f	/* faster: picked up */
#define CAL_STEADY_DPS 8.0f	/* slower for CAL_STEADY_MS: held as in a game */
#define CAL_STEADY_MS 400
#define CAL_TILT_DEG 25.0f

enum cal_step { CAL_OFF, CAL_STILL, CAL_PICK_UP, CAL_STEADY, CAL_TILT };
enum tune_row { TUNE_SENS, TUNE_INVERT_X, TUNE_INVERT_Y, TUNE_ROWS };

struct motion_screen {
	lv_obj_t *box, *cross, *status, *zero, *axes, *note, *bar, *footer;
	int32_t half;		/* of the box */
	lv_timer_t *timer;
	float x, y;		/* the cross: degrees from the middle */
	enum cal_step step;
	struct motion_rec rec;
	float bias[3];
	float step_s;		/* seconds of samples in this step */
	struct input_motion_info shown;
	bool shown_valid;
	lv_obj_t *col;		/* the text column, scrolled to the row chosen */
	lv_obj_t *rows[TUNE_ROWS], *values[TUNE_ROWS];
	int sel;		/* enum tune_row chosen */
	bool tuned;		/* changed: saved when the screen closes */
};

static void footer_hints(struct motion_screen *m)
{
	if (m->step != CAL_OFF)
		footer_set(m->footer, "B", tr(S_CANCEL), NULL);
	else
		footer_set(m->footer, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, tr(S_CHANGE), "A", tr(S_CALIBRATE),
			   "Y", tr(S_RESET), "B", tr(S_BACK), NULL);
}

static void show_tune(struct motion_screen *m)
{
	struct input_motion_tune t;
	int tenths;
	bool inv;

	input_motion_tune(&t);
	tenths = (int)lrintf(t.sensitivity * 10);
	lv_label_set_text_fmt(m->values[TUNE_SENS], "%d.%d\xc3\x97", tenths / 10, tenths % 10);
	lv_obj_set_style_text_color(m->values[TUNE_SENS], tenths == 10 ? lv_color_white() : UI_COLOR_ACCENT, 0);
	for (int r = TUNE_INVERT_X; r <= TUNE_INVERT_Y; r++) {
		inv = r == TUNE_INVERT_X ? t.invert_x : t.invert_y;
		lv_label_set_text(m->values[r], tr(inv ? S_ON : S_OFF));
		lv_obj_set_style_text_color(m->values[r], inv ? UI_COLOR_ACCENT : lv_color_white(), 0);
	}
}

static void select_row(struct motion_screen *m, int sel)
{
	lv_obj_remove_state(m->rows[m->sel], LV_STATE_FOCUSED);
	m->sel = (sel + TUNE_ROWS) % TUNE_ROWS;
	lv_obj_add_state(m->rows[m->sel], LV_STATE_FOCUSED);
	lv_obj_scroll_to_view(m->rows[m->sel], LV_ANIM_ON);
}

/* Left (dir -1) or Right (+1) on the row chosen */
static void change_tune(struct motion_screen *m, int dir)
{
	struct input_motion_tune t;

	input_motion_tune(&t);
	if (m->sel == TUNE_SENS)
		t.sensitivity += dir * 0.1f;
	else if (m->sel == TUNE_INVERT_X)
		t.invert_x = !t.invert_x;
	else
		t.invert_y = !t.invert_y;
	input_set_motion_tune(&t);
	m->tuned = true;
	show_tune(m);
}

static void place_cross(struct motion_screen *m)
{
	int32_t half = m->half, c = ui_px(CROSS_PX) / 2;
	float k = 2 * half / MOTION_BOX_DEG;

	m->x = fmaxf(-MOTION_BOX_DEG / 2, fminf(MOTION_BOX_DEG / 2, m->x));
	m->y = fmaxf(-MOTION_BOX_DEG / 2, fminf(MOTION_BOX_DEG / 2, m->y));
	lv_obj_set_pos(m->cross, half + (int32_t)lrintf(m->x * k) - c, half + (int32_t)lrintf(m->y * k) - c);
}

static void set_note(struct motion_screen *m, enum str_id id, lv_color_t color)
{
	lv_label_set_text(m->note, id < S_COUNT ? tr(id) : "");
	lv_obj_set_style_text_color(m->note, color, 0);
}

static void show_progress(struct motion_screen *m, float part)
{
	if (part < 0) {
		lv_obj_set_hidden(m->bar, true);
		return;
	}
	lv_obj_set_hidden(m->bar, false);
	lv_bar_set_value(m->bar, (int32_t)lrintf(fminf(part, 1) * 100), LV_ANIM_OFF);
}

static void cal_stop(struct motion_screen *m, enum str_id note, lv_color_t color)
{
	input_motion_listen(NULL, NULL);
	m->step = CAL_OFF;
	show_progress(m, -1);
	set_note(m, note, color);
	footer_hints(m);
}

static void cal_finish(struct motion_screen *m)
{
	struct motion_cal c = { .bias_set = true };

	if (!motion_rec_right(&m->rec, c.right)) {
		/* turned about "up": a turn, not the tilt asked for */
		m->step = CAL_PICK_UP;
		m->step_s = 0;
		set_note(m, S_MOTION_CAL_TURNED, UI_COLOR_WARN);
		show_progress(m, 0);
		return;
	}
	c.axes_set = true;
	for (int k = 0; k < 3; k++)
		c.bias[k] = m->bias[k];
	input_set_motion_calibration(&c);
	input_save_calibration();
	m->x = m->y = 0;
	place_cross(m);
	cal_stop(m, S_MOTION_TRY, UI_COLOR_DIM);
	ui_toast(tr(S_MOTION_CAL_DONE));
}

/* Each sample while calibrating (core/input calls it) */
static void on_sample(const float gyro[3], const float accel[3], float dt, void *ud)
{
	struct motion_screen *m = ud;
	float g[3], speed;
	int k;

	for (k = 0; k < 3; k++)
		g[k] = gyro[k] - m->bias[k];
	speed = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
	m->step_s += dt;
	switch (m->step) {
	case CAL_STILL:
		if (!motion_rec_still(&m->rec, gyro, accel)) {
			motion_rec_begin(&m->rec);	/* it moved: again */
			m->step_s = 0;
		}
		show_progress(m, m->step_s * 1000 / CAL_STILL_MS);
		if (m->step_s * 1000 >= CAL_STILL_MS && motion_rec_bias(&m->rec, m->bias)) {
			m->step = CAL_PICK_UP;
			m->step_s = 0;
			set_note(m, S_MOTION_CAL_TILT, lv_color_white());
			show_progress(m, 0);
		}
		break;
	case CAL_PICK_UP:
		if (speed > CAL_PICKED_DPS) {
			m->step = CAL_STEADY;
			m->step_s = 0;
		}
		break;
	case CAL_STEADY:
		if (speed > CAL_STEADY_DPS)
			m->step_s = 0;
		else if (m->step_s * 1000 >= CAL_STEADY_MS) {
			const struct motion *mo = input_motion();

			if (!mo->up_set)
				break;
			motion_rec_tilt_begin(&m->rec, mo->up);
			m->step = CAL_TILT;
			m->step_s = 0;
		}
		break;
	case CAL_TILT: {
		float deg = motion_rec_tilt(&m->rec, gyro, m->bias, dt);

		show_progress(m, deg / CAL_TILT_DEG);
		if (deg >= CAL_TILT_DEG)
			cal_finish(m);
		break;
	}
	case CAL_OFF:
	default:
		break;
	}
}

static void cal_start(struct motion_screen *m)
{
	struct input_motion_info mi;

	input_motion_info(&mi);
	if (!mi.present)
		return;
	m->step = CAL_STILL;
	m->step_s = 0;
	motion_rec_begin(&m->rec);
	set_note(m, S_MOTION_CAL_STILL, lv_color_white());
	show_progress(m, 0);
	footer_hints(m);
	input_motion_listen(on_sample, m);
}

static void on_input(const struct gp_event *ev, void *ud)
{
	struct motion_screen *m = ud;

	if (ev->type == GP_EV_MOTION) {
		if (m->step == CAL_OFF) {
			m->x += ev->mx;
			m->y += ev->my;
			place_cross(m);
		}
		return;
	}
	if (ev->type != GP_EV_BUTTON || !ev->pressed)
		return;
	if (m->step == CAL_OFF && (ev->code == GP_UP || ev->code == GP_DOWN)) {
		select_row(m, m->sel + (ev->code == GP_DOWN ? 1 : -1));
		return;
	}
	if (m->step == CAL_OFF && (ev->code == GP_LEFT || ev->code == GP_RIGHT)) {
		/* the sensitivity steps on while held, a switch once a press */
		if (!ev->repeat || m->sel == TUNE_SENS)
			change_tune(m, ev->code == GP_RIGHT ? 1 : -1);
		return;
	}
	if (ev->repeat)
		return;
	if (m->step != CAL_OFF) {
		if (ev->code == GP_B)
			cal_stop(m, S_COUNT, UI_COLOR_DIM);
		return;
	}
	switch (ev->code) {
	case GP_A:
		cal_start(m);
		break;
	case GP_X:
		m->x = m->y = 0;
		place_cross(m);
		break;
	case GP_Y:
		input_set_motion_calibration(NULL);
		input_save_calibration();
		ui_toast(tr(S_MOTION_CAL_NONE));
		break;
	case GP_B:
		gui_action_post(ACT_BACK, 0);
		break;
	default:
		break;
	}
}

static lv_obj_t *line_new(lv_obj_t *parent)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_width(l, LV_PCT(100));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(l, "");
	return l;
}

/* The sensor's state, as often as STATUS_MS */
static void on_timer(lv_timer_t *t)
{
	struct motion_screen *m = lv_timer_get_user_data(t);
	struct input_motion_info mi;

	input_motion_info(&mi);
	if (m->shown_valid && mi.present == m->shown.present && mi.bias_known == m->shown.bias_known &&
	    mi.axes_set == m->shown.axes_set && fabsf(mi.rate_hz - m->shown.rate_hz) < 1)
		return;
	m->shown = mi;
	m->shown_valid = true;
	if (!mi.present) {
		lv_label_set_text(m->status, tr(S_MOTION_NONE));
		lv_obj_set_style_text_color(m->status, UI_COLOR_WARN, 0);
		lv_obj_set_hidden(m->zero, true);
		lv_obj_set_hidden(m->axes, true);
		if (m->step != CAL_OFF)
			cal_stop(m, S_COUNT, UI_COLOR_DIM);
		return;
	}
	lv_label_set_text_fmt(m->status, tr(S_MOTION_RATE_FMT), (int)lrintf(mi.rate_hz));
	lv_obj_set_style_text_color(m->status, UI_COLOR_ON, 0);
	lv_obj_set_hidden(m->zero, false);
	lv_obj_set_hidden(m->axes, false);
	lv_label_set_text(m->zero, tr(mi.bias_known ? S_MOTION_ZERO_OK : S_MOTION_ZERO_WAIT));
	lv_obj_set_style_text_color(m->zero, mi.bias_known ? UI_COLOR_DIM : UI_COLOR_WARN, 0);
	lv_label_set_text(m->axes, tr(mi.axes_set ? S_MOTION_AXES_OK : S_MOTION_AXES_NONE));
	lv_obj_set_style_text_color(m->axes, mi.axes_set ? UI_COLOR_DIM : UI_COLOR_WARN, 0);
}

static lv_obj_t *cross_new(lv_obj_t *box)
{
	int32_t size = ui_px(CROSS_PX), w = LV_MAX(ui_px(3), 2);
	lv_obj_t *cross = lv_obj_create(box), *bar;

	lv_obj_remove_style_all(cross);
	lv_obj_set_size(cross, size, size);
	lv_obj_set_scrollable(cross, false);
	for (int i = 0; i < 2; i++) {
		bar = lv_obj_create(cross);
		lv_obj_remove_style_all(bar);
		lv_obj_set_size(bar, i ? w : size, i ? size : w);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(bar, UI_COLOR_ACCENT, 0);
		lv_obj_center(bar);
	}
	return cross;
}

static void *motion_screen_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct motion_screen *m = calloc(1, sizeof(*m));
	int32_t side;
	lv_obj_t *scr, *body, *col;
	lv_area_t a;

	(void)parent;
	if (!m)
		return NULL;
	ui_body_area(&a);
	side = LV_MIN(BOX_CELLS * ui_cell(), lv_area_get_height(&a));
	m->half = side / 2;
	scr = console_screen(LV_SYMBOL_GPS, tr(S_MOTION));
	m->footer = footer_create(scr);
	body = screen_body(scr);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(body, ui_cell(), 0);

	m->box = lv_obj_create(body);
	lv_obj_remove_style_all(m->box);
	lv_obj_set_size(m->box, side, side);
	lv_obj_set_style_border_width(m->box, 1, 0);
	lv_obj_set_style_border_color(m->box, UI_COLOR_LINE, 0);
	lv_obj_set_style_radius(m->box, ui_px(6), 0);
	lv_obj_set_scrollable(m->box, false);
	m->cross = cross_new(m->box);
	place_cross(m);

	col = lv_obj_create(body);
	lv_obj_remove_style_all(col);
	lv_obj_set_height(col, LV_PCT(100));
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, ui_cell() / 2, 0);
	lv_obj_set_scroll_dir(col, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
	m->col = col;
	{
		static const enum str_id names[TUNE_ROWS] = { S_SENSITIVITY, S_GYRO_INVERT_X,
							       S_GYRO_INVERT_Y };
		lv_obj_t *rows = lv_obj_create(col), *label;

		lv_obj_remove_style_all(rows);
		lv_obj_set_size(rows, LV_PCT(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_scrollable(rows, false);
		for (int r = 0; r < TUNE_ROWS; r++) {
			lv_obj_t *o = lv_obj_create(rows);

			lv_obj_remove_style_all(o);
			lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
			lv_obj_set_style_pad_hor(o, ui_cell() / 2, 0);
			lv_obj_set_style_pad_ver(o, ui_px(6), 0);
			lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, 0);
			lv_obj_set_style_border_width(o, 1, 0);
			lv_obj_set_style_border_color(o, UI_COLOR_LINE, 0);
			lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
			lv_obj_set_flex_align(o, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
					      LV_FLEX_ALIGN_CENTER);
			lv_obj_set_scrollable(o, false);
			console_focus_box(o);
			label = lv_label_create(o);
			lv_label_set_text(label, tr(names[r]));
			m->values[r] = lv_label_create(o);
			m->rows[r] = o;
		}
		show_tune(m);
		lv_obj_add_state(m->rows[0], LV_STATE_FOCUSED);
	}
	m->status = line_new(col);
	m->zero = line_new(col);
	m->axes = line_new(col);
	m->note = line_new(col);
	m->bar = lv_bar_create(col);
	lv_obj_set_size(m->bar, LV_PCT(100), ui_px(8));
	lv_bar_set_range(m->bar, 0, 100);
	lv_obj_set_style_bg_color(m->bar, UI_COLOR_LINE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(m->bar, UI_COLOR_ACCENT, LV_PART_INDICATOR);
	show_progress(m, -1);
	set_note(m, S_MOTION_TRY, UI_COLOR_DIM);
	footer_hints(m);

	m->timer = lv_timer_create(on_timer, STATUS_MS, m);
	on_timer(m->timer);
	view_input_grab(on_input, m);
	*root = scr;
	return m;
}

static void motion_screen_deinit(void *self)
{
	struct motion_screen *m = self;

	input_motion_listen(NULL, NULL);
	view_input_release(m);
	lv_timer_delete(m->timer);
	if (m->tuned)
		input_save_calibration();
	free(m);
}

const struct widget_ops motion_screen = {
	.name = "motion",
	.init = motion_screen_init,
	.deinit = motion_screen_deinit,
};
