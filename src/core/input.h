/*
 * Logical gamepad (view thread). Drivers report physical buttons and
 * axes over their whole range (joy_linear()); this module calibrates the
 * sticks (joystick.h: rest position, reach of each half axis, the edge
 * made round, a round dead zone), keeps the state, turns the left stick
 * into a virtual D-pad, adds auto-repeat to directions and hands every
 * change to one listener (the view core).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct mainloop;
struct joy_stick_cal;

/* Named as printed on the device */
enum gp_button {
	GP_UP, GP_DOWN, GP_LEFT, GP_RIGHT,
	GP_A, GP_B, GP_X, GP_Y,
	GP_L1, GP_R1, GP_L2, GP_R2,
	GP_SELECT, GP_START, GP_MENU,
	GP_F1, GP_F2, GP_F3, GP_F4, GP_F5, GP_F6,
	GP_VOL_UP, GP_VOL_DOWN,
	GP_POWER,		/* the view core keeps it: sleep */
	GP_COUNT,
	GP_NONE = -1
};
#define GP_BIT(b) (1u << (b))

enum gp_axis { GP_AXIS_LX, GP_AXIS_LY, GP_AXIS_RX, GP_AXIS_RY, GP_AXIS_COUNT };

struct gp_event {
	enum {
		GP_EV_BUTTON, GP_EV_AXIS,
		/* a keyboard and a mouse as they are (input_report_key()) */
		GP_EV_KEY, GP_EV_MOUSE_BUTTON, GP_EV_MOUSE_MOVE,
		/* the motion sensor turned (input_report_motion()): mx, my */
		GP_EV_MOTION,
	} type;
	int code;		/* enum gp_button, enum gp_axis, a Linux KEY_* code, or the
				 * mouse button: 0 left, 1 right, 2 middle */
	bool pressed;		/* buttons, keys */
	bool repeat;		/* auto-repeat of a held direction */
	bool stick;		/* a direction made by the left stick, not the D-pad */
	float value;		/* axes: -1..1 */
	int dx, dy, wheel;	/* GP_EV_MOUSE_MOVE: counts, wheel clicks (up +) */
	float mx, my;		/* GP_EV_MOTION: degrees turned, right and down + */
};

typedef void (*input_cb)(const struct gp_event *ev, void *ud);

struct input_config {
	uint32_t repeat_delay_ms;
	uint32_t repeat_period_ms;
	float stick_press;	/* stick deflection that presses a direction */
	float stick_release;	/* ...and releases it (hysteresis) */
	float stick_deadzone;	/* round, 0..1 of the calibrated reach: the default */
};

void input_init(struct mainloop *ml, const struct input_config *cfg, input_cb cb, void *ud);
void input_deinit(void);

/* From drivers */
void input_report_button(enum gp_button b, bool pressed);
void input_report_axis(enum gp_axis a, float value);	/* joy_linear() units */
void input_report_rest(enum gp_axis a, float value);	/* the axis at rest */
/* Axes moved together (GP_BIT(axis) in mask; an evdev SYN_REPORT): a stick
 * is calibrated once with both of its new values, never half moved */
void input_report_axes(const float value[GP_AXIS_COUNT], uint32_t mask);

/*
 * Keyboards and mice (drivers/input_hid.c). While raw(false) says so (a
 * game in front that takes a keyboard, as Windows games and Heroes III do)
 * keys go to the listener as they are: GP_EV_KEY with Linux key codes;
 * otherwise the keys of input_key_button() press gamepad buttons (arrows,
 * Enter A, Esc B...). While raw(true) says so (a game that takes a mouse)
 * the mouse goes to the listener as GP_EV_MOUSE_*, otherwise it does
 * nothing. A release goes where its press went.
 */
void input_set_raw_query(bool (*raw)(bool mouse));
void input_report_key(unsigned code, bool pressed);
void input_report_mouse_button(int button, bool pressed);
void input_report_mouse_move(int dx, int dy, int wheel);
enum gp_button input_key_button(unsigned code);	/* GP_NONE: no button */

uint32_t input_buttons(void);		/* GP_BIT() mask of pressed buttons */
uint32_t input_buttons_physical(void);	/* ...less the left stick's directions */
float input_axis(enum gp_axis a);	/* calibrated, -1..1 */

/*
 * Stick calibration: stick 0 is the left one (LX/LY), 1 the right one.
 * Without one (NULL) the stick's rest position and the driver's range are
 * used. The file has a line per calibrated stick: "left = <joy_stick_format()>",
 * and its dead zone and sensitivity (input_set_stick_tune()).
 */
#define INPUT_STICKS 2
void input_raw_stick(int stick, float *x, float *y);	/* before calibration */
bool input_calibration(int stick, struct joy_stick_cal *c);	/* false: none, c the default */
void input_set_calibration(int stick, const struct joy_stick_cal *c);
int input_load_calibration(const char *path);	/* "~/" is $HOME; NULL: memory only */
int input_save_calibration(void);		/* to the file loaded from */

/*
 * Dead zone and sensitivity of each stick, set in the input test and
 * saved with the calibration ("left.deadzone = 0.15", "left.sensitivity =
 * 1.00", "left.snapback = 1"). The dead zone is round, a part of the
 * calibrated reach; the sensitivity multiplies the position past it
 * (joy_stick_gain()); snap-back puts a stick that creeps home slowly after
 * being let go at rest at once (joy_snap_update()). Games and the stick's
 * D-pad get the result.
 */
#define INPUT_DEADZONE_MAX 0.5f
#define INPUT_SENS_MIN 0.5f
#define INPUT_SENS_MAX 2.0f

struct input_stick_tune {
	float deadzone, sensitivity;
	bool snapback;
};

void input_stick_tune(int stick, struct input_stick_tune *t);
void input_stick_tune_default(struct input_stick_tune *t);
void input_set_stick_tune(int stick, const struct input_stick_tune *t);	/* NULL: default */

/*
 * A motion sensor (drivers/input_imu.c; core/motion.h does the math). The
 * driver reports each sample, then input_report_motion_end() after a batch
 * of them hands the listener what the device turned meanwhile (GP_EV_MOTION;
 * only once it knows the gyro's zero). The driver describes the sensor as
 * the device says it (input_motion_sensor(): an id "<chip>-<serial>", its
 * channels and noise); nothing here knows a chip. Each sensor has a
 * calibration of its own, kept in the sticks' file ("gyro.<id> =
 * <motion_cal_format()>"). A sensor without a gyro gets one made from
 * gravity (motion_synth_gyro()), one without an accelerometer aims in its
 * own calibrated axes (core/motion.h).
 */
#define INPUT_MOTION_ID_LEN 64
#define INPUT_MOTION_CALS 8
struct motion;
struct motion_cal;

struct input_motion_info {
	bool present;		/* a sensor is plugged in */
	char name[17];		/* as the device names it, "" unknown */
	float rate_hz;		/* its samples a second */
	bool bias_known;	/* the gyro's zero measured (lain still) or calibrated */
	bool axes_set;		/* calibrated: up and down (and, without an accelerometer, left and right) */
	unsigned channels;	/* MOTION_CH_* the sensor has */
	bool aim_x, aim_y;	/* it turns the view left and right, up and down now */
	bool still;		/* it lay still the last second */
	float rate_x, rate_y;	/* deg/s now: right, down */
};

void input_motion_present(bool present, float rate_hz);
/* Which sensor it is, as the device says: another id (or other channels)
 * starts afresh: its zero, its calibration */
struct input_motion_desc {
	const char *id;		/* [A-Za-z0-9_-] */
	const char *name;	/* for the user */
	unsigned channels;	/* MOTION_CH_GYRO, MOTION_CH_ACCEL */
	float gyro_noise;	/* deg/s rms at rest, 0 unknown */
	float accel_noise;	/* g rms at rest, 0 unknown */
	float rate_hz;		/* samples a second, nominal; 0 unknown */
};
void input_motion_sensor(const struct input_motion_desc *d);

/*
 * The sensor as a whole, for every game (the motion screen sets it, saved
 * with the calibration: "gyro.sensitivity = 1.0", "gyro.invert_x = 0",
 * "gyro.invert_y = 0"): GP_EV_MOTION is turned by it before the listener
 * sees it. A game's controls have a sensitivity of their own on top.
 */
#define INPUT_GYRO_SENS_MIN 0.2f
#define INPUT_GYRO_SENS_MAX 5.0f

struct input_motion_tune {
	float sensitivity;
	bool invert_x, invert_y;
};

void input_motion_tune(struct input_motion_tune *t);
void input_set_motion_tune(const struct input_motion_tune *t);	/* NULL: the default */
/* The stream had a gap (samples lost for a while), or the port was opened
 * again: what came before is stale */
void input_motion_gap(void);
/* deg/s, g, s; a channel the sensor lacks is ignored (0) */
void input_report_motion(const float gyro[3], const float accel[3], float dt);
void input_report_motion_end(void);
void input_motion_info(struct input_motion_info *mi);
const struct motion *input_motion(void);	/* for "up" and the bias */
/* Every sample as it comes (the calibration), NULL: none */
typedef void (*input_motion_cb)(const float gyro[3], const float accel[3], float dt, void *ud);
void input_motion_listen(input_motion_cb cb, void *ud);
bool input_motion_calibration(struct motion_cal *c);	/* the sensor's; false: none */
void input_set_motion_calibration(const struct motion_cal *c);	/* the sensor's; NULL: none */

/*
 * Input devices plugged in now, for the status bar and the settings: the
 * drivers say what they have (input_report_devices(which, present): the
 * bits of 'which' set as 'present' says), the listener hears every change.
 */
#define INPUT_DEV_KEYBOARD (1u << 0)
#define INPUT_DEV_MOUSE (1u << 1)
#define INPUT_DEV_MOTION (1u << 2)

void input_report_devices(uint32_t which, uint32_t present);
uint32_t input_devices(void);
void input_set_devices_cb(void (*cb)(uint32_t devices));

/* What the hardware has: drivers declare it when a device appears */
void input_declare(uint32_t buttons, uint32_t axes);	/* GP_BIT() masks, added up */
uint32_t input_buttons_present(void);
uint32_t input_axes_present(void);

const char *gp_button_name(enum gp_button b);
enum gp_button gp_button_from_name(const char *name);
