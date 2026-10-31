/* Host unit tests for the toolkit independent core. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/input.h"
#include "core/joystick.h"
#include "core/mainloop.h"
#include "core/msgq.h"
#include "core/uevent.h"
#include "gui/action.h"
#include "gui/control/control.h"
#include "gui/ctx.h"
#include "services/config.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

static void test_joystick(void)
{
	/* OGA rev 1.1 X axis: 172..772, resting off-center */
	struct joy_axis_cal c = { .min = 172, .max = 772, .center = 500, .deadzone = 0.1f };

	CHECK(NEAR(joy_normalize(&c, 500), 0));
	CHECK(NEAR(joy_normalize(&c, 772), 1));
	CHECK(NEAR(joy_normalize(&c, 172), -1));
	CHECK(NEAR(joy_normalize(&c, 900), 1));		/* clamped */
	CHECK(NEAR(joy_normalize(&c, 520), 0));		/* inside dead zone */
	/* half way up (636) is 0.5 raw -> (0.5 - 0.1) / 0.9 after the dead zone */
	CHECK(NEAR(joy_normalize(&c, 636), 0.4f / 0.9f));
	c.invert = true;
	CHECK(NEAR(joy_normalize(&c, 772), -1));

	CHECK(joy_direction(0, 0.6f, 0.5f, 0.3f) == 1);
	CHECK(joy_direction(1, 0.4f, 0.5f, 0.3f) == 1);		/* hysteresis holds */
	CHECK(joy_direction(1, 0.2f, 0.5f, 0.3f) == 0);
	CHECK(joy_direction(0, 0.4f, 0.5f, 0.3f) == 0);
	CHECK(joy_direction(1, -0.6f, 0.5f, 0.3f) == -1);	/* flick to the other side */
	CHECK(joy_direction(-1, -0.4f, 0.5f, 0.3f) == -1);
}

static void test_msgq(void)
{
	struct msgq *q = msgq_new(sizeof(int), 3);
	int v, i;

	for (i = 1; i <= 3; i++)
		CHECK(msgq_post(q, &i));
	i = 4;
	CHECK(!msgq_post(q, &i));		/* full */
	for (i = 1; i <= 3; i++) {
		CHECK(msgq_pop(q, &v));
		CHECK(v == i);			/* FIFO */
	}
	CHECK(!msgq_pop(q, &v));
	msgq_free(q);
}

static void test_ctx(void)
{
	struct gui_ctx snap;
	struct gui_ctx *c;

	CHECK(ctx_init() == 0);
	CHECK(ctx_snapshot(&snap) == CTX_ALL);	/* first snapshot: everything */
	CHECK(ctx_snapshot(&snap) == 0);

	/* two writes before the view looks: one change, latest value */
	c = ctx_lock();
	c->power.capacity = 50;
	ctx_unlock(CTX_POWER);
	c = ctx_lock();
	c->power.capacity = 49;
	c->display.brightness = 70;
	ctx_unlock(CTX_POWER | CTX_DISPLAY);
	CHECK(ctx_snapshot(&snap) == (CTX_POWER | CTX_DISPLAY));
	CHECK(snap.power.capacity == 49);
	CHECK(snap.display.brightness == 70);
	ctx_deinit();
}

static void test_nav(void)
{
	struct control_env env = { 0 };
	struct gui_action a;
	struct gui_ctx snap;

	CHECK(ctx_init() == 0);
	ctl_nav.init(&env);
	ctx_snapshot(&snap);
	CHECK(ctx_screen(&snap) == SCREEN_HOME);

	a = (struct gui_action){ .type = ACT_BACK, .arg = 0 };
	ctl_nav.on_action(&a);			/* home has nothing to go back to */
	CHECK(ctx_snapshot(&snap) == 0);

	a = (struct gui_action){ .type = ACT_OPEN_SCREEN, .arg = SCREEN_SETTINGS };
	ctl_nav.on_action(&a);
	ctl_nav.on_action(&a);			/* same screen twice: one entry */
	CHECK(ctx_snapshot(&snap) == CTX_NAV);
	CHECK(ctx_screen(&snap) == SCREEN_SETTINGS && snap.nav.depth == 2);

	a = (struct gui_action){ .type = ACT_OPEN_SCREEN, .arg = SCREEN_COUNT };
	ctl_nav.on_action(&a);			/* invalid id ignored */
	CHECK(ctx_snapshot(&snap) == 0);

	a = (struct gui_action){ .type = ACT_BACK, .arg = 0 };
	ctl_nav.on_action(&a);
	CHECK(ctx_snapshot(&snap) == CTX_NAV);
	CHECK(ctx_screen(&snap) == SCREEN_HOME && snap.nav.depth == 1);
	ctx_deinit();
}

static void test_uevent(void)
{
	static const char msg[] =
		"add@/devices/platform/adc-joystick/input/input5/event5\0"
		"ACTION=add\0DEVPATH=/devices/platform/adc-joystick/input/input5/event5\0"
		"SUBSYSTEM=input\0MAJOR=13\0MINOR=69\0DEVNAME=input/event5\0SEQNUM=1234";
	static const char udev[] = "libudev\0\xfe\xed\xca\xfe";
	char buf[sizeof(msg)];
	struct uevent ev;

	memcpy(buf, msg, sizeof(msg));
	CHECK(uevent_parse(buf, sizeof(buf), &ev) == 0);
	CHECK(!strcmp(ev.action, "add") && !strcmp(ev.subsystem, "input"));
	CHECK(ev.devname && !strcmp(ev.devname, "input/event5"));
	CHECK(ev.name == NULL);

	memcpy(buf, udev, sizeof(udev));
	CHECK(uevent_parse(buf, sizeof(udev), &ev) < 0);	/* udev monitor format ignored */
}

static void test_config(void)
{
	const char *path = "test_config.conf";
	struct mainloop *ml = mainloop_new("test");
	FILE *f;

	unlink(path);
	config_open(ml, path);
	CHECK(config_get("lang") == NULL);
	CHECK(config_get_int("brightness", -1) == -1);
	config_set("lang", "ru");
	config_set_int("brightness", 55);
	config_close();				/* writes the delayed save now */

	f = fopen(path, "r");
	CHECK(f != NULL);
	if (f)
		fclose(f);
	config_open(ml, path);
	CHECK(config_get("lang") && !strcmp(config_get("lang"), "ru"));
	CHECK(config_get_int("brightness", -1) == 55);
	config_close();
	unlink(path);

	config_open(ml, NULL);			/* memory only */
	config_set("lang", "en");
	CHECK(!strcmp(config_get("lang"), "en"));
	config_close();
	mainloop_free(ml);
}

/* ---- input: repeat and stick -> D-pad, driven by a real loop ---------- */

static struct {
	int down, up, repeats;
	enum gp_button last;
} seen;

static void on_input(const struct gp_event *ev, void *ud)
{
	(void)ud;
	if (ev->type != GP_EV_BUTTON)
		return;
	seen.last = ev->code;
	if (ev->repeat)
		seen.repeats++;
	else if (ev->pressed)
		seen.down++;
	else
		seen.up++;
}

static void release_right(void *ud)
{
	input_report_button(GP_RIGHT, false);
	mainloop_quit(ud);
}

static void test_input(void)
{
	struct input_config cfg = { .repeat_delay_ms = 50, .repeat_period_ms = 20,
				    .stick_press = 0.5f, .stick_release = 0.3f };
	struct mainloop *ml = mainloop_new("test");

	input_init(ml, &cfg, on_input, NULL);

	/* stick: past press -> UP, inside hysteresis -> still UP, back -> released */
	input_report_axis(GP_AXIS_LY, -0.7f);
	CHECK(seen.down == 1 && seen.last == GP_UP && (input_buttons() & GP_BIT(GP_UP)));
	input_report_axis(GP_AXIS_LY, -0.4f);
	CHECK(seen.up == 0);
	input_report_axis(GP_AXIS_LY, 0);
	CHECK(seen.up == 1 && !(input_buttons() & GP_BIT(GP_UP)));

	/* D-pad held stays pressed even when the stick lets go of the same direction */
	input_report_button(GP_UP, true);
	input_report_axis(GP_AXIS_LY, -0.9f);
	input_report_axis(GP_AXIS_LY, 0);
	CHECK(input_buttons() & GP_BIT(GP_UP));
	input_report_button(GP_UP, false);

	/* held direction repeats: 50 ms delay, then every 20 ms; hold for 130 ms */
	memset(&seen, 0, sizeof(seen));
	input_report_button(GP_RIGHT, true);
	mainloop_add_timer(ml, 130, 0, release_right, ml);
	mainloop_run(ml);
	CHECK(seen.down == 1 && seen.up == 1);
	CHECK(seen.repeats >= 3 && seen.repeats <= 5);

	input_deinit();
	mainloop_free(ml);
}

int main(void)
{
	test_joystick();
	test_msgq();
	test_ctx();
	test_nav();
	test_uevent();
	test_config();
	test_input();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all core tests passed");
	return 0;
}
