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
	} type;
	int code;		/* enum gp_button, enum gp_axis, a Linux KEY_* code, or the
				 * mouse button: 0 left, 1 right, 2 middle */
	bool pressed;		/* buttons, keys */
	bool repeat;		/* auto-repeat of a held direction */
	bool stick;		/* a direction made by the left stick, not the D-pad */
	float value;		/* axes: -1..1 */
	int dx, dy, wheel;	/* GP_EV_MOUSE_MOVE: counts, wheel clicks (up +) */
};

typedef void (*input_cb)(const struct gp_event *ev, void *ud);

struct input_config {
	uint32_t repeat_delay_ms;
	uint32_t repeat_period_ms;
	float stick_press;	/* stick deflection that presses a direction */
	float stick_release;	/* ...and releases it (hysteresis) */
	float stick_deadzone;	/* round, 0..1 of the calibrated reach */
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
float input_axis(enum gp_axis a);	/* calibrated, -1..1 */

/*
 * Stick calibration: stick 0 is the left one (LX/LY), 1 the right one.
 * Without one (NULL) the stick's rest position and the driver's range are
 * used. The file has a line per calibrated stick: "left = <joy_stick_format()>".
 */
#define INPUT_STICKS 2
void input_raw_stick(int stick, float *x, float *y);	/* before calibration */
bool input_calibration(int stick, struct joy_stick_cal *c);	/* false: none, c the default */
void input_set_calibration(int stick, const struct joy_stick_cal *c);
int input_load_calibration(const char *path);	/* "~/" is $HOME; NULL: memory only */
int input_save_calibration(void);		/* to the file loaded from */

/* What the hardware has: drivers declare it when a device appears */
void input_declare(uint32_t buttons, uint32_t axes);	/* GP_BIT() masks, added up */
uint32_t input_buttons_present(void);
uint32_t input_axes_present(void);

const char *gp_button_name(enum gp_button b);
enum gp_button gp_button_from_name(const char *name);
