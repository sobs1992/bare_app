#include "input.h"

#include <errno.h>
#include <math.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "joystick.h"
#include "log.h"
#include "mainloop.h"
#include "motion.h"
#include "util.h"

#define TAG "input"
#define KEY_SLOTS 256		/* key codes kept track of: a keyboard's are below */

#define DIR_MASK (GP_BIT(GP_UP) | GP_BIT(GP_DOWN) | GP_BIT(GP_LEFT) | GP_BIT(GP_RIGHT))
/* L2/R2 are volume keys in the UI and in Heroes III (games never get repeats) */
#define REPEAT_MASK (DIR_MASK | GP_BIT(GP_VOL_UP) | GP_BIT(GP_VOL_DOWN) | GP_BIT(GP_L2) | \
		     GP_BIT(GP_R2))

static const char *const names[GP_COUNT] = {
	[GP_UP] = "up", [GP_DOWN] = "down", [GP_LEFT] = "left", [GP_RIGHT] = "right",
	[GP_A] = "a", [GP_B] = "b", [GP_X] = "x", [GP_Y] = "y",
	[GP_L1] = "l1", [GP_R1] = "r1", [GP_L2] = "l2", [GP_R2] = "r2",
	[GP_SELECT] = "select", [GP_START] = "start", [GP_MENU] = "menu",
	[GP_F1] = "f1", [GP_F2] = "f2", [GP_F3] = "f3",
	[GP_F4] = "f4", [GP_F5] = "f5", [GP_F6] = "f6",
	[GP_VOL_UP] = "vol+", [GP_VOL_DOWN] = "vol-", [GP_POWER] = "power",
};

static struct {
	struct mainloop *ml;
	struct input_config cfg;
	input_cb cb;
	void *ud;
	uint32_t phys;		/* reported by drivers */
	uint32_t virt;		/* from the stick */
	uint32_t state;		/* phys | virt, what listeners saw */
	uint32_t from_stick;	/* of 'state': pressed by the stick alone */
	float axes[GP_AXIS_COUNT];	/* calibrated */
	float raw[GP_AXIS_COUNT], rest[GP_AXIS_COUNT];	/* joy_linear() units */
	struct {
		bool set;
		struct joy_stick_cal cal;
		struct input_stick_tune tune;
		struct joy_snap snap;
	} stick[INPUT_STICKS];
	char cal_path[256];
	uint32_t buttons_present, axes_present;
	int stick_x, stick_y;	/* -1/0/+1 */
	struct ml_timer *repeat;
	enum gp_button repeat_btn;
	bool (*raw_query)(bool mouse);
	uint8_t key_raw[KEY_SLOTS / 8], key_btn[KEY_SLOTS / 8];	/* where presses went */
	uint8_t mouse_raw;	/* bit per mouse button pressed raw */
	struct {
		bool present;
		float rate_hz;
		struct motion m;
		bool cal_set;	/* m.cal came from the file or the calibration */
		char id[INPUT_MOTION_ID_LEN];	/* the sensor now ("" none yet) */
		char name[17];
		unsigned channels;
		float noise, accel_noise;
		struct motion_synth synth;	/* the gyro of a sensor without one */
		/* calibrations by sensor id; "" the one from before sensors had ids */
		struct {
			char id[INPUT_MOTION_ID_LEN];
			struct motion_cal cal;
		} cals[INPUT_MOTION_CALS];
		int ncals;
		struct input_motion_tune tune;
		input_motion_cb cb;
		void *ud;
	} motion;
	uint32_t devices;	/* INPUT_DEV_* */
	void (*devices_cb)(uint32_t devices);
} in;

static void emit_button(enum gp_button b, bool pressed, bool repeat)
{
	struct gp_event ev = { .type = GP_EV_BUTTON, .code = b, .pressed = pressed, .repeat = repeat,
			       .stick = in.from_stick & GP_BIT(b) };

	if (in.cb)
		in.cb(&ev, in.ud);
}

static void on_repeat(void *ud)
{
	(void)ud;
	if (in.state & GP_BIT(in.repeat_btn))
		emit_button(in.repeat_btn, true, true);
}

static void stop_repeat(void)
{
	mainloop_timer_cancel(in.ml, in.repeat);
	in.repeat = NULL;
}

/* Bring 'state' to phys | virt, emitting an event per changed button */
static void sync_state(void)
{
	uint32_t want = in.phys | in.virt, diff = want ^ in.state;
	int b;

	for (b = 0; b < GP_COUNT; b++) {
		bool pressed;

		if (!(diff & GP_BIT(b)))
			continue;
		pressed = want & GP_BIT(b);
		in.state ^= GP_BIT(b);
		/* a release keeps the source of its press */
		if (pressed && !(in.phys & GP_BIT(b)))
			in.from_stick |= GP_BIT(b);
		else if (pressed)
			in.from_stick &= ~GP_BIT(b);
		if (GP_BIT(b) & REPEAT_MASK) {
			if (pressed) {
				/* the latest direction (or volume key) repeats */
				stop_repeat();
				in.repeat_btn = b;
				in.repeat = mainloop_add_timer(in.ml, in.cfg.repeat_delay_ms,
							       in.cfg.repeat_period_ms, on_repeat, NULL);
			} else if (in.repeat && in.repeat_btn == (enum gp_button)b) {
				stop_repeat();
			}
		}
		emit_button(b, pressed, false);
		if (!pressed)
			in.from_stick &= ~GP_BIT(b);
	}
}

void input_init(struct mainloop *ml, const struct input_config *cfg, input_cb cb, void *ud)
{
	static const struct input_config def = {
		.repeat_delay_ms = 400,
		.repeat_period_ms = 120,
		.stick_press = 0.5f,
		.stick_release = 0.3f,
		.stick_deadzone = 0.15f,
	};

	int s;

	memset(&in, 0, sizeof(in));
	in.ml = ml;
	in.cfg = cfg ? *cfg : def;
	in.cb = cb;
	in.ud = ud;
	for (s = 0; s < INPUT_STICKS; s++)
		input_stick_tune_default(&in.stick[s].tune);
	motion_init(&in.motion.m);
	input_set_motion_tune(NULL);
}

void input_deinit(void)
{
	stop_repeat();
	in.cb = NULL;
}

void input_report_button(enum gp_button b, bool pressed)
{
	if (b < 0 || b >= GP_COUNT)
		return;
	if (pressed)
		in.phys |= GP_BIT(b);
	else
		in.phys &= ~GP_BIT(b);
	sync_state();
}

static void stick_to_dpad(void)
{
	int x = joy_direction(in.stick_x, in.axes[GP_AXIS_LX], in.cfg.stick_press, in.cfg.stick_release);
	int y = joy_direction(in.stick_y, in.axes[GP_AXIS_LY], in.cfg.stick_press, in.cfg.stick_release);

	if (x == in.stick_x && y == in.stick_y)
		return;
	in.stick_x = x;
	in.stick_y = y;
	in.virt = (x < 0 ? GP_BIT(GP_LEFT) : 0) | (x > 0 ? GP_BIT(GP_RIGHT) : 0) |
		  (y < 0 ? GP_BIT(GP_UP) : 0) | (y > 0 ? GP_BIT(GP_DOWN) : 0);
	sync_state();
}

static void set_axis(enum gp_axis a, float value)
{
	struct gp_event ev = { .type = GP_EV_AXIS, .code = a, .value = value };

	if (in.axes[a] == value)
		return;
	in.axes[a] = value;
	if (in.cb)
		in.cb(&ev, in.ud);
}

static const struct joy_stick_cal *stick_cal(int s, struct joy_stick_cal *def)
{
	if (in.stick[s].set)
		return &in.stick[s].cal;
	joy_stick_default(def, in.rest[2 * s], in.rest[2 * s + 1]);
	return def;
}

/* Both axes of a stick move together: the edge and the dead zone are round */
static void update_stick(int s)
{
	struct joy_stick_cal def;
	float x, y;

	joy_stick_apply(stick_cal(s, &def), in.stick[s].tune.deadzone, in.raw[2 * s],
			in.raw[2 * s + 1], &x, &y);
	if (joy_snap_update(&in.stick[s].snap, now_ms(), hypotf(x, y)) && in.stick[s].tune.snapback)
		x = y = 0;
	joy_stick_gain(in.stick[s].tune.sensitivity, &x, &y);
	set_axis(2 * s, x);
	set_axis(2 * s + 1, y);
	if (s == 0)
		stick_to_dpad();
}

void input_report_axis(enum gp_axis a, float value)
{
	if (a < 0 || a >= GP_AXIS_COUNT || in.raw[a] == value)
		return;
	in.raw[a] = value;
	update_stick(a / 2);
}

void input_report_axes(const float value[GP_AXIS_COUNT], uint32_t mask)
{
	uint32_t moved = 0;
	int a, s;

	for (a = 0; a < GP_AXIS_COUNT; a++)
		if ((mask & GP_BIT(a)) && in.raw[a] != value[a]) {
			in.raw[a] = value[a];
			moved |= GP_BIT(a / 2);
		}
	for (s = 0; s < INPUT_STICKS; s++)
		if (moved & GP_BIT(s))
			update_stick(s);
}

void input_report_rest(enum gp_axis a, float value)
{
	if (a < 0 || a >= GP_AXIS_COUNT)
		return;
	in.rest[a] = in.raw[a] = value;
	update_stick(a / 2);
}

void input_raw_stick(int stick, float *x, float *y)
{
	bool ok = stick >= 0 && stick < INPUT_STICKS;

	*x = ok ? in.raw[2 * stick] : 0;
	*y = ok ? in.raw[2 * stick + 1] : 0;
}

bool input_calibration(int stick, struct joy_stick_cal *c)
{
	if (stick < 0 || stick >= INPUT_STICKS)
		return false;
	*c = *stick_cal(stick, c);
	return in.stick[stick].set;
}

void input_set_calibration(int stick, const struct joy_stick_cal *c)
{
	if (stick < 0 || stick >= INPUT_STICKS)
		return;
	in.stick[stick].set = c != NULL;
	if (c)
		in.stick[stick].cal = *c;
	update_stick(stick);
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void input_stick_tune_default(struct input_stick_tune *t)
{
	t->deadzone = clampf(in.cfg.stick_deadzone, 0, INPUT_DEADZONE_MAX);
	t->sensitivity = 1;
	t->snapback = false;
}

void input_stick_tune(int stick, struct input_stick_tune *t)
{
	if (stick < 0 || stick >= INPUT_STICKS)
		input_stick_tune_default(t);
	else
		*t = in.stick[stick].tune;
}

void input_set_stick_tune(int stick, const struct input_stick_tune *t)
{
	struct input_stick_tune *to;

	if (stick < 0 || stick >= INPUT_STICKS)
		return;
	to = &in.stick[stick].tune;
	if (!t) {
		input_stick_tune_default(to);
	} else {
		to->deadzone = clampf(t->deadzone, 0, INPUT_DEADZONE_MAX);
		to->sensitivity = clampf(t->sensitivity, INPUT_SENS_MIN, INPUT_SENS_MAX);
		to->snapback = t->snapback;
	}
	update_stick(stick);
}

static const char *const stick_names[INPUT_STICKS] = { "left", "right" };

/* "left.deadzone = 0.15": a stick's dead zone, sensitivity or snap-back */
static bool load_tune_line(int s, const char *field, const char *value)
{
	struct input_stick_tune t = in.stick[s].tune;
	char *end;
	float v = strtof(value, &end);

	while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
		end++;
	if (end == value || *end)
		return false;
	if (!strcmp(field, "deadzone"))
		t.deadzone = v;
	else if (!strcmp(field, "sensitivity"))
		t.sensitivity = v;
	else if (!strcmp(field, "snapback"))
		t.snapback = v != 0;
	else
		return false;
	input_set_stick_tune(s, &t);
	return true;
}

static int cal_find(const char *id)
{
	for (int i = 0; i < in.motion.ncals; i++)
		if (!strcmp(in.motion.cals[i].id, id))
			return i;
	return -1;
}

/* The calibration of sensor id: kept, replaced or (c NULL) dropped */
static bool cal_store(const char *id, const struct motion_cal *c)
{
	int i = cal_find(id);

	if (!c || !(c->bias_set || c->axes_set || c->up_set)) {
		if (i >= 0)
			in.motion.cals[i] = in.motion.cals[--in.motion.ncals];
		return true;
	}
	if (i < 0) {
		if (in.motion.ncals == INPUT_MOTION_CALS || strlen(id) >= INPUT_MOTION_ID_LEN) {
			LOGW(TAG, "no room for the calibration of motion sensor %s", id);
			return false;
		}
		i = in.motion.ncals++;
		snprintf(in.motion.cals[i].id, INPUT_MOTION_ID_LEN, "%s", id);
	}
	in.motion.cals[i].cal = *c;
	return true;
}

/* The sensor's calibration into core/motion */
static void cal_apply(void)
{
	int i = in.motion.id[0] ? cal_find(in.motion.id) : -1;	/* no sensor named yet: none */

	in.motion.cal_set = i >= 0;
	motion_set_cal(&in.motion.m, i >= 0 ? &in.motion.cals[i].cal : NULL);
}

static bool id_ok(const char *id)
{
	for (; *id; id++)
		if (!((*id >= '0' && *id <= '9') || (*id >= 'A' && *id <= 'Z') ||
		      (*id >= 'a' && *id <= 'z') || *id == '-' || *id == '_'))
			return false;
	return true;
}

/*
 * The sensor's lines: "gyro.sensitivity = 1.0", "gyro.invert_x = 1" (the
 * tune), "gyro.<sensor id> = <motion_cal_format()>" (its calibration)
 */
static bool load_gyro_line(const char *field, const char *value)
{
	struct input_motion_tune t = in.motion.tune;
	char *end;
	float v;

	if (strcmp(field, "sensitivity") && strcmp(field, "invert_x") && strcmp(field, "invert_y")) {
		struct motion_cal mc;

		if (!*field || !id_ok(field) || !motion_cal_parse(value, &mc) || !cal_store(field, &mc))
			return false;
		cal_apply();
		return true;
	}
	v = strtof(value, &end);

	while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
		end++;
	if (end == value || *end || !isfinite(v))
		return false;
	if (!strcmp(field, "sensitivity"))
		t.sensitivity = v;
	else if (!strcmp(field, "invert_x"))
		t.invert_x = v != 0;
	else if (!strcmp(field, "invert_y"))
		t.invert_y = v != 0;
	else
		return false;
	input_set_motion_tune(&t);
	return true;
}

int input_load_calibration(const char *path)
{
	const char *home = getenv("HOME");
	char line[512];
	FILE *f;

	in.cal_path[0] = '\0';
	if (!path)
		return 0;
	if (!strncmp(path, "~/", 2)) {
		if (!home)
			return 0;
		snprintf(in.cal_path, sizeof(in.cal_path), "%s/%s", home, path + 2);
	} else {
		snprintf(in.cal_path, sizeof(in.cal_path), "%s", path);
	}
	if (!(f = fopen(in.cal_path, "r"))) {
		if (errno != ENOENT)
			LOGW(TAG, "%s: %s", in.cal_path, strerror(errno));
		return errno == ENOENT ? 0 : -1;
	}
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '='), *key = line, *dot;
		struct joy_stick_cal c;
		int s;

		if (*line == '#' || !eq)
			continue;
		*eq = '\0';
		while (*key == ' ')
			key++;
		for (char *e = eq; e > key && e[-1] == ' '; )
			*--e = '\0';
		if (!strcmp(key, "gyro") || !strncmp(key, "gyro.", 5)) {
			if (!load_gyro_line(key[4] ? key + 5 : "", eq + 1))
				LOGW(TAG, "%s: bad line \"%s\"", in.cal_path, key);
			continue;
		}
		dot = strchr(key, '.');
		if (dot)
			*dot = '\0';
		for (s = 0; s < INPUT_STICKS; s++)
			if (!strcmp(key, stick_names[s]))
				break;
		if (dot) {
			if (s == INPUT_STICKS || !load_tune_line(s, dot + 1, eq + 1))
				LOGW(TAG, "%s: bad line \"%s.%s\"", in.cal_path, key, dot + 1);
			continue;
		}
		if (s == INPUT_STICKS || !joy_stick_parse(eq + 1, &c)) {
			LOGW(TAG, "%s: bad line \"%s\"", in.cal_path, key);
			continue;
		}
		input_set_calibration(s, &c);
		LOGI(TAG, "%s stick calibrated", stick_names[s]);
	}
	fclose(f);
	return 0;
}

int input_save_calibration(void)
{
	char tmp[sizeof(in.cal_path) + 8], buf[400];
	FILE *f;
	int s, err = 0;

	if (!in.cal_path[0])
		return 0;
	snprintf(tmp, sizeof(tmp), "%s.tmp", in.cal_path);
	if (!(f = fopen(tmp, "w"))) {
		LOGW(TAG, "%s: %s", tmp, strerror(errno));
		return -1;
	}
	fputs("# bare_app stick calibration, written by the input test\n", f);
	for (s = 0; s < INPUT_STICKS; s++) {
		if (in.stick[s].set && joy_stick_format(&in.stick[s].cal, buf, sizeof(buf)) > 0)
			fprintf(f, "%s = %s\n", stick_names[s], buf);
		fprintf(f, "%s.deadzone = %.2f\n%s.sensitivity = %.2f\n%s.snapback = %d\n",
			stick_names[s], (double)in.stick[s].tune.deadzone, stick_names[s],
			(double)in.stick[s].tune.sensitivity, stick_names[s],
			in.stick[s].tune.snapback);
	}
	for (int i = 0; i < in.motion.ncals; i++)
		if (motion_cal_format(&in.motion.cals[i].cal, buf, sizeof(buf)) > 0)
			fprintf(f, "gyro%s%s = %s\n", in.motion.cals[i].id[0] ? "." : "",
				in.motion.cals[i].id, buf);
	fprintf(f, "gyro.sensitivity = %.1f\ngyro.invert_x = %d\ngyro.invert_y = %d\n",
		(double)in.motion.tune.sensitivity, in.motion.tune.invert_x, in.motion.tune.invert_y);
	if (fflush(f) || fsync(fileno(f)))
		err = -1;
	if (fclose(f) || err || rename(tmp, in.cal_path)) {
		LOGW(TAG, "%s: %s", in.cal_path, strerror(errno));
		unlink(tmp);
		return -1;
	}
	LOGI(TAG, "saved %s", in.cal_path);
	return 0;
}

/* ---- the motion sensor -------------------------------------------------- */

void input_motion_present(bool present, float rate_hz)
{
	if (present && !in.motion.present)
		motion_drop(&in.motion.m);
	in.motion.present = present;
	in.motion.rate_hz = present ? rate_hz : 0;
	input_report_devices(INPUT_DEV_MOTION, present ? INPUT_DEV_MOTION : 0);
}

void input_motion_tune(struct input_motion_tune *t)
{
	*t = in.motion.tune;
}

void input_set_motion_tune(const struct input_motion_tune *t)
{
	struct input_motion_tune *to = &in.motion.tune;

	if (!t) {
		*to = (struct input_motion_tune){ .sensitivity = 1 };
		return;
	}
	*to = *t;
	if (!isfinite(to->sensitivity))
		to->sensitivity = 1;
	/* tenths, as the screen steps it */
	to->sensitivity = roundf(clampf(t->sensitivity, INPUT_GYRO_SENS_MIN, INPUT_GYRO_SENS_MAX) * 10) / 10;
}

void input_report_devices(uint32_t which, uint32_t present)
{
	uint32_t devices = (in.devices & ~which) | (present & which);

	if (devices == in.devices)
		return;
	in.devices = devices;
	if (in.devices_cb)
		in.devices_cb(devices);
}

uint32_t input_devices(void)
{
	return in.devices;
}

void input_set_devices_cb(void (*cb)(uint32_t devices))
{
	in.devices_cb = cb;
}

void input_motion_sensor(const struct input_motion_desc *d)
{
	const char *id = d->id && id_ok(d->id) && strlen(d->id) < INPUT_MOTION_ID_LEN ? d->id : "unknown";
	unsigned ch = d->channels & (MOTION_CH_GYRO | MOTION_CH_ACCEL);

	if (!strcmp(id, in.motion.id) && ch == in.motion.channels && d->gyro_noise == in.motion.noise &&
	    d->accel_noise == in.motion.accel_noise)
		return;
	if (strcmp(id, in.motion.id) || ch != in.motion.channels) {
		/* another sensor: its zero, its "up", its calibration */
		LOGI(TAG, "motion sensor %s:%s%s%s", id, ch & MOTION_CH_GYRO ? " gyro" : "",
		     ch & MOTION_CH_ACCEL ? " accelerometer" : "", ch ? "" : " nothing to aim with");
		motion_init(&in.motion.m);
		memset(&in.motion.synth, 0, sizeof(in.motion.synth));
	}
	snprintf(in.motion.id, sizeof(in.motion.id), "%s", id);
	snprintf(in.motion.name, sizeof(in.motion.name), "%s", d->name ? d->name : "");
	in.motion.channels = ch;
	in.motion.noise = d->gyro_noise;
	in.motion.accel_noise = d->accel_noise;
	motion_set_sensor(&in.motion.m, ch, d->gyro_noise, d->accel_noise);
	cal_apply();
}

void input_report_motion(const float gyro[3], const float accel[3], float dt)
{
	static const float none[3];
	float made[3];

	if (in.motion.id[0] && !in.motion.channels)
		return;		/* nothing to aim with */
	if (in.motion.id[0] && !(in.motion.channels & MOTION_CH_ACCEL))
		accel = none;
	if (in.motion.id[0] && !(in.motion.channels & MOTION_CH_GYRO)) {
		motion_synth_gyro(&in.motion.synth, accel, dt, made);
		gyro = made;
	}
	motion_feed(&in.motion.m, gyro, accel, dt);
	if (in.motion.cb)
		in.motion.cb(gyro, accel, dt, in.motion.ud);
}

void input_report_motion_end(void)
{
	struct gp_event ev = { .type = GP_EV_MOTION };

	motion_take(&in.motion.m, &ev.mx, &ev.my);
	ev.mx *= in.motion.tune.sensitivity * (in.motion.tune.invert_x ? -1 : 1);
	ev.my *= in.motion.tune.sensitivity * (in.motion.tune.invert_y ? -1 : 1);
	if ((ev.mx != 0 || ev.my != 0) && in.cb)
		in.cb(&ev, in.ud);
}

void input_motion_info(struct input_motion_info *mi)
{
	const struct motion *m = &in.motion.m;

	*mi = (struct input_motion_info){
		.present = in.motion.present, .rate_hz = in.motion.rate_hz,
		.bias_known = m->bias_known, .still = m->still, .channels = m->channels,
		.axes_set = m->cal.axes_set && (m->cal.up_set || (m->channels & MOTION_CH_ACCEL)),
		.rate_x = in.motion.present ? m->rate_x : 0, .rate_y = in.motion.present ? m->rate_y : 0,
	};
	motion_can_aim(m, &mi->aim_x, &mi->aim_y);
	snprintf(mi->name, sizeof(mi->name), "%s", in.motion.name);
}

const struct motion *input_motion(void)
{
	return &in.motion.m;
}

void input_motion_listen(input_motion_cb cb, void *ud)
{
	in.motion.cb = cb;
	in.motion.ud = ud;
}

bool input_motion_calibration(struct motion_cal *c)
{
	*c = in.motion.m.cal;
	return in.motion.cal_set;
}

void input_set_motion_calibration(const struct motion_cal *c)
{
	if (in.motion.id[0])
		cal_store(in.motion.id, c);
	cal_apply();
}

uint32_t input_buttons(void)
{
	return in.state;
}

uint32_t input_buttons_physical(void)
{
	return in.phys;
}

float input_axis(enum gp_axis a)
{
	return a >= 0 && a < GP_AXIS_COUNT ? in.axes[a] : 0;
}

void input_declare(uint32_t buttons, uint32_t axes)
{
	in.buttons_present |= buttons;
	in.axes_present |= axes;
}

uint32_t input_buttons_present(void)
{
	return in.buttons_present;
}

uint32_t input_axes_present(void)
{
	return in.axes_present;
}

const char *gp_button_name(enum gp_button b)
{
	return b >= 0 && b < GP_COUNT ? names[b] : "?";
}

enum gp_button gp_button_from_name(const char *name)
{
	int b;

	for (b = 0; b < GP_COUNT; b++)
		if (!strcmp(names[b], name))
			return b;
	return GP_NONE;
}

/* ---- keyboards and mice ------------------------------------------------ */

static const struct { uint16_t code; enum gp_button button; } key_buttons[] = {
	{ KEY_UP, GP_UP }, { KEY_DOWN, GP_DOWN }, { KEY_LEFT, GP_LEFT }, { KEY_RIGHT, GP_RIGHT },
	{ KEY_ENTER, GP_A }, { KEY_KPENTER, GP_A }, { KEY_ESC, GP_B }, { KEY_BACKSPACE, GP_B },
	{ KEY_SPACE, GP_X }, { KEY_TAB, GP_Y },
	{ KEY_PAGEUP, GP_L1 }, { KEY_PAGEDOWN, GP_R1 },
	{ KEY_F1, GP_F1 }, { KEY_F2, GP_F2 }, { KEY_F3, GP_F3 },
	{ KEY_F4, GP_F4 }, { KEY_F5, GP_F5 }, { KEY_F6, GP_F6 },
	{ KEY_VOLUMEUP, GP_VOL_UP }, { KEY_VOLUMEDOWN, GP_VOL_DOWN },
};

enum gp_button input_key_button(unsigned code)
{
	for (size_t i = 0; i < sizeof(key_buttons) / sizeof(key_buttons[0]); i++)
		if (key_buttons[i].code == code)
			return key_buttons[i].button;
	return GP_NONE;
}

void input_set_raw_query(bool (*raw)(bool mouse))
{
	in.raw_query = raw;
}

static bool raw_now(bool mouse)
{
	return in.raw_query && in.raw_query(mouse);
}

#define BIT_SET(a, i) ((a)[(i) / 8] |= 1u << ((i) % 8))
#define BIT_CLR(a, i) ((a)[(i) / 8] &= ~(1u << ((i) % 8)))
#define BIT_GET(a, i) ((a)[(i) / 8] >> ((i) % 8) & 1u)

void input_report_key(unsigned code, bool pressed)
{
	struct gp_event ev = { .type = GP_EV_KEY, .code = (int)code, .pressed = pressed };
	enum gp_button b = input_key_button(code);

	if (code >= KEY_SLOTS)
		return;
	if (pressed) {
		if (raw_now(false)) {
			BIT_SET(in.key_raw, code);
			if (in.cb)
				in.cb(&ev, in.ud);
		} else if (b != GP_NONE) {
			BIT_SET(in.key_btn, code);
			input_report_button(b, true);
		}
		return;
	}
	if (BIT_GET(in.key_raw, code)) {
		BIT_CLR(in.key_raw, code);
		if (in.cb)
			in.cb(&ev, in.ud);
	}
	if (BIT_GET(in.key_btn, code)) {
		BIT_CLR(in.key_btn, code);
		input_report_button(b, false);
	}
}

void input_report_mouse_button(int button, bool pressed)
{
	struct gp_event ev = { .type = GP_EV_MOUSE_BUTTON, .code = button, .pressed = pressed };

	if (button < 0 || button > 7)
		return;
	if (pressed) {
		if (!raw_now(true))
			return;
		in.mouse_raw |= 1u << button;
	} else if (in.mouse_raw & 1u << button) {
		in.mouse_raw &= ~(1u << button);
	} else {
		return;
	}
	if (in.cb)
		in.cb(&ev, in.ud);
}

void input_report_mouse_move(int dx, int dy, int wheel)
{
	struct gp_event ev = { .type = GP_EV_MOUSE_MOVE, .dx = dx, .dy = dy, .wheel = wheel };

	if ((dx || dy || wheel) && raw_now(true) && in.cb)
		in.cb(&ev, in.ud);
}
