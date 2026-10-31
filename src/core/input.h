/*
 * Logical gamepad (view thread). Drivers report physical buttons and
 * normalised axes; this module keeps the state, turns the left stick into
 * a virtual D-pad, adds auto-repeat to directions and hands every change
 * to one listener (the view core).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct mainloop;

/* Named as printed on the device */
enum gp_button {
	GP_UP, GP_DOWN, GP_LEFT, GP_RIGHT,
	GP_A, GP_B, GP_X, GP_Y,
	GP_L1, GP_R1, GP_L2, GP_R2,
	GP_SELECT, GP_START, GP_MENU,
	GP_F1, GP_F2, GP_F3, GP_F4, GP_F5, GP_F6,
	GP_VOL_UP, GP_VOL_DOWN,
	GP_COUNT,
	GP_NONE = -1
};
#define GP_BIT(b) (1u << (b))

enum gp_axis { GP_AXIS_LX, GP_AXIS_LY, GP_AXIS_RX, GP_AXIS_RY, GP_AXIS_COUNT };

struct gp_event {
	enum { GP_EV_BUTTON, GP_EV_AXIS } type;
	int code;		/* enum gp_button or enum gp_axis */
	bool pressed;		/* buttons */
	bool repeat;		/* auto-repeat of a held direction */
	float value;		/* axes: -1..1 */
};

typedef void (*input_cb)(const struct gp_event *ev, void *ud);

struct input_config {
	uint32_t repeat_delay_ms;
	uint32_t repeat_period_ms;
	float stick_press;	/* stick deflection that presses a direction */
	float stick_release;	/* ...and releases it (hysteresis) */
};

void input_init(struct mainloop *ml, const struct input_config *cfg, input_cb cb, void *ud);
void input_deinit(void);

/* From drivers */
void input_report_button(enum gp_button b, bool pressed);
void input_report_axis(enum gp_axis a, float value);

uint32_t input_buttons(void);		/* GP_BIT() mask of pressed buttons */
float input_axis(enum gp_axis a);

const char *gp_button_name(enum gp_button b);
enum gp_button gp_button_from_name(const char *name);
