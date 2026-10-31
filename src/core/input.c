#include "input.h"

#include <string.h>

#include "joystick.h"
#include "log.h"
#include "mainloop.h"
#include "util.h"

#define DIR_MASK (GP_BIT(GP_UP) | GP_BIT(GP_DOWN) | GP_BIT(GP_LEFT) | GP_BIT(GP_RIGHT))
#define REPEAT_MASK (DIR_MASK | GP_BIT(GP_VOL_UP) | GP_BIT(GP_VOL_DOWN))

static const char *const names[GP_COUNT] = {
	[GP_UP] = "up", [GP_DOWN] = "down", [GP_LEFT] = "left", [GP_RIGHT] = "right",
	[GP_A] = "a", [GP_B] = "b", [GP_X] = "x", [GP_Y] = "y",
	[GP_L1] = "l1", [GP_R1] = "r1", [GP_L2] = "l2", [GP_R2] = "r2",
	[GP_SELECT] = "select", [GP_START] = "start", [GP_MENU] = "menu",
	[GP_F1] = "f1", [GP_F2] = "f2", [GP_F3] = "f3",
	[GP_F4] = "f4", [GP_F5] = "f5", [GP_F6] = "f6",
	[GP_VOL_UP] = "vol+", [GP_VOL_DOWN] = "vol-",
};

static struct {
	struct mainloop *ml;
	struct input_config cfg;
	input_cb cb;
	void *ud;
	uint32_t phys;		/* reported by drivers */
	uint32_t virt;		/* from the stick */
	uint32_t state;		/* phys | virt, what listeners saw */
	float axes[GP_AXIS_COUNT];
	uint32_t buttons_present, axes_present;
	int stick_x, stick_y;	/* -1/0/+1 */
	struct ml_timer *repeat;
	enum gp_button repeat_btn;
} in;

static void emit_button(enum gp_button b, bool pressed, bool repeat)
{
	struct gp_event ev = { .type = GP_EV_BUTTON, .code = b, .pressed = pressed, .repeat = repeat };

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
	}
}

void input_init(struct mainloop *ml, const struct input_config *cfg, input_cb cb, void *ud)
{
	static const struct input_config def = {
		.repeat_delay_ms = 400,
		.repeat_period_ms = 120,
		.stick_press = 0.5f,
		.stick_release = 0.3f,
	};

	memset(&in, 0, sizeof(in));
	in.ml = ml;
	in.cfg = cfg ? *cfg : def;
	in.cb = cb;
	in.ud = ud;
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

void input_report_axis(enum gp_axis a, float value)
{
	struct gp_event ev = { .type = GP_EV_AXIS, .code = a, .value = value };

	if (a < 0 || a >= GP_AXIS_COUNT || in.axes[a] == value)
		return;
	in.axes[a] = value;
	if (in.cb)
		in.cb(&ev, in.ud);
	if (a == GP_AXIS_LX || a == GP_AXIS_LY)
		stick_to_dpad();
}

uint32_t input_buttons(void)
{
	return in.state;
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
