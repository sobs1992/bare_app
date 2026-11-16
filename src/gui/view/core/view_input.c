/*
 * The view core's gamepad (view_priv.h): what every event goes through
 * before a screen or the toolkit sees it. The power key (sleep, holding
 * it turns off), the quit combo (held: quits, over a game ends the game),
 * F5 held for the system monitor, the volume keys; then the screen that
 * grabbed the gamepad, or UI keys for the backend.
 */
#include "view_priv.h"

#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "core/mainloop.h"
#include "core/util.h"
#include "gui/action.h"

#define TAG "view"
#define HOLD_MS 2000		/* the quit combo and the power key act once held this long */
#define POWER_SHORT_MS 500	/* a longer press of the power key starts turning off */
#define SCREEN_OFF_MS 10000	/* asleep with the screen off this long: real sleep */
#define SYSMON_HOLD_MS 600	/* F5 held this long shows or hides the system monitor */

/* ---- input ------------------------------------------------------------ */

void view_input_grab(view_grab_cb cb, void *ud)
{
	vcore.grab = cb;
	vcore.grab_ud = ud;
}

void view_input_release(void *ud)
{
	if (vcore.grab_ud == ud) {
		vcore.grab = NULL;
		vcore.grab_ud = NULL;
	}
}

static int button_to_key(enum gp_button b)
{
	switch (b) {
	case GP_UP:    return UI_KEY_UP;
	case GP_DOWN:  return UI_KEY_DOWN;
	case GP_LEFT:  return UI_KEY_LEFT;
	case GP_RIGHT: return UI_KEY_RIGHT;
	case GP_A:     return UI_KEY_ACCEPT;
	case GP_B:     return UI_KEY_BACK;
	case GP_L1:    return UI_KEY_PREV;
	case GP_R1:    return UI_KEY_NEXT;
	case GP_START: return UI_KEY_MENU;
	case GP_X:     return UI_KEY_ALT;
	default:       return -1;
	}
}

void view_notify_backend(enum notify_kind kind, int value)
{
	struct gui_notify n = { kind, value };

	if (vcore.be->on_notify)
		vcore.be->on_notify(&n);
}


static void on_quit_hold(void *ud)
{
	(void)ud;
	vcore.quit_hold = NULL;
	if (vcore.quit_game) {
		view_notify_backend(NOTIFY_GAME_QUIT_HOLD, 0);
		view_game_quit();
		return;
	}
	vcore.quit_sent = true;
	gui_action_post(ACT_QUIT, 0);
}

/* Quitting takes holding the combo: the backend warns meanwhile. Over a
 * running game the combo ends the game, not the application. A device
 * (a platform that powers off) is not left by it: the application is all
 * there is on it, and the power key turns it off. */
static void quit_combo_changed(void)
{
	uint32_t combo = vcore.pf->quit_combo;
	bool held = (input_buttons() & combo) == combo;

	if (held && !vcore.quit_hold && vcore.pf->poweroff && !view_game_in_front())
		return;
	if (held && !vcore.quit_hold && !vcore.quit_sent) {
		vcore.quit_game = view_game_in_front();
		vcore.quit_hold = mainloop_add_timer(vcore.ml, HOLD_MS, 0, on_quit_hold, NULL);
		view_notify_backend(vcore.quit_game ? NOTIFY_GAME_QUIT_HOLD : NOTIFY_QUIT_HOLD, HOLD_MS);
	} else if (!held && vcore.quit_hold) {
		mainloop_timer_cancel(vcore.ml, vcore.quit_hold);
		vcore.quit_hold = NULL;
		view_notify_backend(vcore.quit_game ? NOTIFY_GAME_QUIT_HOLD : NOTIFY_QUIT_HOLD, 0);
	}
}

/*
 * The power key. A short press turns the screen off and takes all other
 * input away; a press brings the screen back. Ten seconds later the device
 * really sleeps (the platform's suspend command) until the power key wakes
 * it. That wake press arrives once it is awake again: everything until
 * shortly after NOTIFY_RESUMED is ignored. Holding the key turns the
 * device off: past POWER_SHORT_MS the backend warns, letting go then
 * cancels; at HOLD_MS the screen goes dark, the sign to let go.
 */
void view_screen_on(void)
{
	if (vcore.deep_sleep)
		mainloop_timer_cancel(vcore.ml, vcore.deep_sleep);
	vcore.deep_sleep = NULL;
	vcore.screen_off = false;
	gui_action_post(ACT_SCREEN_OFF, 0);
}

static void on_deep_sleep(void *ud)
{
	(void)ud;
	vcore.deep_sleep = NULL;
	vcore.asleep = true;
	gui_action_post(ACT_SUSPEND, 0);
}

static void screen_off(void)
{
	vcore.screen_off = true;
	gui_action_post(ACT_SCREEN_OFF, 1);
	if (vcore.pf->suspend)
		vcore.deep_sleep = mainloop_add_timer(vcore.ml, SCREEN_OFF_MS, 0, on_deep_sleep, NULL);
}

static void on_power_hold(void *ud)
{
	(void)ud;
	if (!vcore.power_warned) {
		vcore.power_warned = true;
		vcore.power_hold = mainloop_add_timer(vcore.ml, HOLD_MS - POWER_SHORT_MS, 0, on_power_hold, NULL);
		view_notify_backend(NOTIFY_POWER_HOLD, HOLD_MS - POWER_SHORT_MS);
		return;
	}
	vcore.power_hold = NULL;
	vcore.power_down = false;
	vcore.quit_sent = true;
	/* dark at once: the key can be let go, the device turns off by
	 * itself (held on, the PMIC's own long press may cut the power
	 * before the file systems are put away) */
	gui_action_post(ACT_SCREEN_OFF, 1);
	gui_action_post(ACT_POWER_OFF, 0);
}

static void power_key(const struct gp_event *ev)
{
	if (ev->repeat)
		return;
	if (vcore.asleep || now_ms() < vcore.power_quiet_until || vcore.quit_sent)
		return;
	if (ev->pressed) {
		if (vcore.screen_off) {
			view_screen_on();	/* this press is done: its release does nothing */
		} else if (!vcore.power_down) {
			vcore.power_down = true;
			vcore.power_warned = false;
			vcore.power_hold = mainloop_add_timer(vcore.ml, POWER_SHORT_MS, 0, on_power_hold, NULL);
		}
		return;
	}
	if (!vcore.power_down)
		return;
	vcore.power_down = false;
	mainloop_timer_cancel(vcore.ml, vcore.power_hold);
	vcore.power_hold = NULL;
	if (vcore.power_warned)
		view_notify_backend(NOTIFY_POWER_HOLD, 0);	/* let go: not turning off */
	else
		screen_off();
}

static void deliver(const struct gp_event *ev);

#ifdef CONFIG_SYSMON
static void on_f5_hold(void *ud)
{
	enum sysmon_mode mode;

	(void)ud;
	vcore.f5_hold = NULL;
	vcore.f5_held = true;
	/* hidden -> everything -> over a game only its frame times -> hidden */
	if (!vcore.snap.sysmon.shown)
		mode = SYSMON_FULL;
	else if (!vcore.snap.sysmon.frames_only && view_video_shown() != VIDEO_NONE)
		mode = SYSMON_FRAMES;
	else
		mode = SYSMON_HIDDEN;
	gui_action_post(ACT_SYSMON, mode);
}

/*
 * F5 held shows or hides the system monitor, over everything, also over
 * a game that grabbed the gamepad. Its press is kept back until it is
 * clear which one it is: a short one goes on as press and release when
 * F5 is let go (minimise a game, its own meaning in a game).
 */
static void f5_key(const struct gp_event *ev)
{
	struct gp_event press;

	if (ev->repeat)
		return;
	if (ev->pressed) {
		if (!vcore.f5_down) {
			vcore.f5_down = true;
			vcore.f5_held = false;
			vcore.f5_hold = mainloop_add_timer(vcore.ml, SYSMON_HOLD_MS, 0, on_f5_hold, NULL);
		}
		return;
	}
	if (!vcore.f5_down) {
		deliver(ev);	/* a release whose press was not seen here */
		return;
	}
	vcore.f5_down = false;
	if (vcore.f5_hold)
		mainloop_timer_cancel(vcore.ml, vcore.f5_hold);
	vcore.f5_hold = NULL;
	if (vcore.f5_held)
		return;
	press = *ev;
	press.pressed = true;
	deliver(&press);
	deliver(ev);
}
#endif

void view_on_gamepad(const struct gp_event *ev, void *ud)
{
	uint32_t combo = vcore.pf->quit_combo;

	(void)ud;
	if (ev->type == GP_EV_KEY || ev->type == GP_EV_MOUSE_BUTTON || ev->type == GP_EV_MOUSE_MOVE) {
		/* a keyboard and a mouse as they are: only a game in front gets
		 * them (core/input asked view_hid_game_in_front()); presses not while
		 * the screen is off, releases always */
		if ((vcore.screen_off || vcore.asleep) && (ev->type == GP_EV_MOUSE_MOVE || ev->pressed))
			return;
		view_video_input(ev);
		return;
	}
	if (ev->type == GP_EV_MOTION) {
		/* to the screen that grabbed the gamepad: a game turns its view by
		 * it (screens/game.c), the motion screen shows it */
		if (!vcore.screen_off && !vcore.asleep && vcore.grab)
			vcore.grab(ev, vcore.grab_ud);
		return;
	}
	if (ev->type == GP_EV_BUTTON) {
		LOGD(TAG, "%s %s%s", gp_button_name(ev->code), ev->pressed ? "down" : "up",
		     ev->repeat ? " (repeat)" : "");
		/* both belong to the view core, also over a game that grabbed the gamepad */
		if (ev->code == GP_POWER) {
			power_key(ev);
			return;
		}
	}
	/* Nobody sees what presses would do. Releases still go: a button held
	 * when the screen went off must not stay pressed in a game. */
	if ((vcore.screen_off || vcore.asleep) && (ev->type == GP_EV_BUTTON ? ev->pressed : ev->value != 0))
		return;
	if (ev->type == GP_EV_BUTTON) {
		if (combo && (GP_BIT(ev->code) & combo)) {
			if (!ev->repeat)
				quit_combo_changed();
			return;
		}
		/* volume keys work everywhere, also in a game that grabbed the gamepad */
		if (ev->pressed && (ev->code == GP_VOL_UP || ev->code == GP_VOL_DOWN))
			gui_action_post(ACT_VOLUME_STEP, ev->code == GP_VOL_UP ? VIEW_VOLUME_STEP
									 : -VIEW_VOLUME_STEP);
#ifdef CONFIG_SYSMON
		if (ev->code == GP_F5 && vcore.snap.sysmon.available) {
			f5_key(ev);
			return;
		}
#endif
	}
	deliver(ev);
}

/* To the screen that grabbed the gamepad, or as UI keys */
static void deliver(const struct gp_event *ev)
{
	int key;

	if (vcore.grab) {
		vcore.grab(ev, vcore.grab_ud);
		return;
	}
	if (ev->type != GP_EV_BUTTON)
		return;
	if (ev->code == GP_F5 || ev->code == view_game_minimize_button()) {
		if (ev->pressed && !ev->repeat)
			view_game_unminimize();
		return;
	}
	/* the triggers mean nothing to the UI: volume, as in games that leave them */
	if (ev->code == GP_L2 || ev->code == GP_R2) {
		if (ev->pressed)
			gui_action_post(ACT_VOLUME_STEP, ev->code == GP_R2 ? VIEW_VOLUME_STEP
									   : -VIEW_VOLUME_STEP);
		return;
	}
	key = button_to_key(ev->code);
	if (key < 0)
		return;
	if (!vcore.be->on_key(key, ev->pressed, ev->repeat) && key == UI_KEY_BACK && ev->pressed &&
	    !ev->repeat)
		gui_action_post(ACT_BACK, 0);
}

const char *ui_key_name(enum ui_key key)
{
	static const char *const names[UI_KEY_COUNT] = {
		"up", "down", "left", "right", "accept", "back", "prev", "next", "menu", "alt",
	};

	return key < UI_KEY_COUNT ? names[key] : "?";
}
