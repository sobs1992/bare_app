#include "view.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/frametimes.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "core/util.h"
#include "gui/action.h"
#include "hal/platform.h"
#include "i18n.h"
#include "media/mailbox.h"
#include "media/testpattern.h"

#define TAG "view"
#define MAX_SUBS 128
#define HOLD_MS 2000		/* the quit combo and the power key act once held this long */
#define POWER_SHORT_MS 500	/* a longer press of the power key starts turning off */
#define SCREEN_OFF_MS 10000	/* asleep with the screen off this long: real sleep */
#define POWER_QUIET_MS 1000	/* after waking up the key's events are the wake press */
#define SYSMON_HOLD_MS 600	/* F5 held this long shows or hides the system monitor */

struct vsub {
	int id;			/* 0: free */
	uint32_t sections;
	view_cb cb;
	void *self;
};

static struct {
	const struct view_backend_ops *be;
	const struct platform *pf;
	pthread_t thread;
	sem_t ready;
	int status;
	struct mainloop *ml;
	struct hal_display *display;
	void *inputs[PLATFORM_MAX_INPUTS];
	struct gui_ctx snap;
	struct vsub subs[MAX_SUBS];
	int nsubs, next_id;
	view_grab_cb grab;
	void *grab_ud;
	bool quit_sent;
	struct ml_timer *quit_hold;	/* the quit combo is held: quits when it fires */
	bool quit_game;			/* ...ends the game in front instead */
	struct ml_timer *power_hold;	/* the power key is down: warns, then turns off */
	bool power_down;		/* a press of it that counts is going on */
	bool power_warned;		/* ...and has gone past POWER_SHORT_MS */
	struct ml_timer *deep_sleep;	/* the screen is off: real sleep when it fires */
	bool screen_off;		/* first stage of sleep: no input but the power key */
	bool asleep;			/* ACT_SUSPEND posted, NOTIFY_RESUMED not yet */
	uint64_t power_quiet_until;	/* now_ms() */
	struct ml_timer *f5_hold;	/* F5 is down: the monitor when it fires */
	bool f5_down;			/* a press of F5 kept back from the rest */
	bool f5_held;			/* ...that went on long enough: not a press */
	enum video_source video_src;
	struct mailbox *video_mb;
	struct testpattern *video_tp;
	struct game_proc *video_game;	/* VIDEO_GAME: the process of 'game' */
	struct game_ref game, game_sel;	/* in the layer; to start next */
	bool video_minimized;		/* video_game is stopped in the background */
	unsigned video_frames;
	struct frametimes frame_times;	/* of video_frames, since it was set or came back */
	struct {			/* hitches since the source was set */
		uint64_t last_us;	/* the last frame, 0: none yet */
		unsigned over50, over100;	/* frames later than that many ms */
		uint32_t worst_us;
		uint64_t log_us;	/* BARE_APP_FRAME_STATS: the last line, 0 off */
	} frame_stats;
	view_video_end_cb video_end;
	void *video_end_ud;
} v;

/* ---- subscriptions ---------------------------------------------------- */

int view_subscribe(uint32_t sections, view_cb cb, void *self)
{
	int i;

	for (i = 0; i < v.nsubs; i++)
		if (!v.subs[i].id)
			break;
	if (i == v.nsubs) {
		if (v.nsubs == MAX_SUBS) {
			LOGE(TAG, "too many subscriptions");
			return -1;
		}
		v.nsubs++;
	}
	v.subs[i] = (struct vsub){ ++v.next_id, sections, cb, self };
	cb(self, &v.snap, CTX_ALL);
	return v.subs[i].id;
}

void view_unsubscribe(int id)
{
	int i;

	for (i = 0; i < v.nsubs; i++)
		if (v.subs[i].id == id)
			v.subs[i].id = 0;
}

const struct gui_ctx *view_ctx(void)
{
	return &v.snap;
}

static void dispatch(uint32_t changed)
{
	int i, n = v.nsubs, id;

	/*
	 * Callbacks may subscribe (new screen) or unsubscribe (deleted
	 * screen). New entries are past 'n' or reuse freed slots with a newer
	 * id; both already got their first call from view_subscribe().
	 */
	id = v.next_id;
	for (i = 0; i < n; i++) {
		struct vsub *s = &v.subs[i];

		if (s->id && s->id <= id && (s->sections & changed))
			s->cb(s->self, &v.snap, changed & s->sections);
	}
}

/* ---- input ------------------------------------------------------------ */

void view_input_grab(view_grab_cb cb, void *ud)
{
	v.grab = cb;
	v.grab_ud = ud;
}

void view_input_release(void *ud)
{
	if (v.grab_ud == ud) {
		v.grab = NULL;
		v.grab_ud = NULL;
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

static void notify_backend(enum notify_kind kind, int value)
{
	struct gui_notify n = { kind, value };

	if (v.be->on_notify)
		v.be->on_notify(&n);
}

static bool game_in_front(void);
static void game_quit(void);
static void game_unminimize(void);

static void on_quit_hold(void *ud)
{
	(void)ud;
	v.quit_hold = NULL;
	if (v.quit_game) {
		notify_backend(NOTIFY_GAME_QUIT_HOLD, 0);
		game_quit();
		return;
	}
	v.quit_sent = true;
	gui_action_post(ACT_QUIT, 0);
}

/* Quitting takes holding the combo: the backend warns meanwhile. Over a
 * running game the combo ends the game, not the application. */
static void quit_combo_changed(void)
{
	uint32_t combo = v.pf->quit_combo;
	bool held = (input_buttons() & combo) == combo;

	if (held && !v.quit_hold && !v.quit_sent) {
		v.quit_game = game_in_front();
		v.quit_hold = mainloop_add_timer(v.ml, HOLD_MS, 0, on_quit_hold, NULL);
		notify_backend(v.quit_game ? NOTIFY_GAME_QUIT_HOLD : NOTIFY_QUIT_HOLD, HOLD_MS);
	} else if (!held && v.quit_hold) {
		mainloop_timer_cancel(v.ml, v.quit_hold);
		v.quit_hold = NULL;
		notify_backend(v.quit_game ? NOTIFY_GAME_QUIT_HOLD : NOTIFY_QUIT_HOLD, 0);
	}
}

/*
 * The power key. A short press turns the screen off and takes all other
 * input away; a press brings the screen back. Ten seconds later the device
 * really sleeps (the platform's suspend command) until the power key wakes
 * it. That wake press arrives once it is awake again: everything until
 * shortly after NOTIFY_RESUMED is ignored. Holding the key turns the
 * device off: past POWER_SHORT_MS the backend warns, letting go then
 * cancels.
 */
static void screen_on(void)
{
	if (v.deep_sleep)
		mainloop_timer_cancel(v.ml, v.deep_sleep);
	v.deep_sleep = NULL;
	v.screen_off = false;
	gui_action_post(ACT_SCREEN_OFF, 0);
}

static void on_deep_sleep(void *ud)
{
	(void)ud;
	v.deep_sleep = NULL;
	v.asleep = true;
	gui_action_post(ACT_SUSPEND, 0);
}

static void screen_off(void)
{
	v.screen_off = true;
	gui_action_post(ACT_SCREEN_OFF, 1);
	if (v.pf->suspend)
		v.deep_sleep = mainloop_add_timer(v.ml, SCREEN_OFF_MS, 0, on_deep_sleep, NULL);
}

static void on_power_hold(void *ud)
{
	(void)ud;
	if (!v.power_warned) {
		v.power_warned = true;
		v.power_hold = mainloop_add_timer(v.ml, HOLD_MS - POWER_SHORT_MS, 0, on_power_hold, NULL);
		notify_backend(NOTIFY_POWER_HOLD, HOLD_MS - POWER_SHORT_MS);
		return;
	}
	v.power_hold = NULL;
	v.power_down = false;
	v.quit_sent = true;
	gui_action_post(ACT_POWER_OFF, 0);	/* the warning stays until the end */
}

static void power_key(const struct gp_event *ev)
{
	if (ev->repeat)
		return;
	if (v.asleep || now_ms() < v.power_quiet_until || v.quit_sent)
		return;
	if (ev->pressed) {
		if (v.screen_off) {
			screen_on();	/* this press is done: its release does nothing */
		} else if (!v.power_down) {
			v.power_down = true;
			v.power_warned = false;
			v.power_hold = mainloop_add_timer(v.ml, POWER_SHORT_MS, 0, on_power_hold, NULL);
		}
		return;
	}
	if (!v.power_down)
		return;
	v.power_down = false;
	mainloop_timer_cancel(v.ml, v.power_hold);
	v.power_hold = NULL;
	if (v.power_warned)
		notify_backend(NOTIFY_POWER_HOLD, 0);	/* let go: not turning off */
	else
		screen_off();
}

static void deliver(const struct gp_event *ev);

#ifdef CONFIG_SYSMON
static void on_f5_hold(void *ud)
{
	enum sysmon_mode mode;

	(void)ud;
	v.f5_hold = NULL;
	v.f5_held = true;
	/* hidden -> everything -> over a game only its frame times -> hidden */
	if (!v.snap.sysmon.shown)
		mode = SYSMON_FULL;
	else if (!v.snap.sysmon.frames_only && view_video_shown() != VIDEO_NONE)
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
		if (!v.f5_down) {
			v.f5_down = true;
			v.f5_held = false;
			v.f5_hold = mainloop_add_timer(v.ml, SYSMON_HOLD_MS, 0, on_f5_hold, NULL);
		}
		return;
	}
	if (!v.f5_down) {
		deliver(ev);	/* a release whose press was not seen here */
		return;
	}
	v.f5_down = false;
	if (v.f5_hold)
		mainloop_timer_cancel(v.ml, v.f5_hold);
	v.f5_hold = NULL;
	if (v.f5_held)
		return;
	press = *ev;
	press.pressed = true;
	deliver(&press);
	deliver(ev);
}
#endif

static void on_gamepad(const struct gp_event *ev, void *ud)
{
	uint32_t combo = v.pf->quit_combo;

	(void)ud;
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
	if ((v.screen_off || v.asleep) && (ev->type == GP_EV_BUTTON ? ev->pressed : ev->value != 0))
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
		if (ev->code == GP_F5 && v.snap.sysmon.available) {
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

	if (v.grab) {
		v.grab(ev, v.grab_ud);
		return;
	}
	if (ev->type != GP_EV_BUTTON)
		return;
	if (ev->code == GP_F5 || ev->code == view_game_minimize_button()) {
		if (ev->pressed && !ev->repeat)
			game_unminimize();
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
	if (!v.be->on_key(key, ev->pressed, ev->repeat) && key == UI_KEY_BACK && ev->pressed &&
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

/* ---- video layer ------------------------------------------------------ */

static struct hal_layer *video_layer(void)
{
	return v.display ? hal_display_layer(v.display, LAYER_VIDEO) : NULL;
}

bool view_video_available(void)
{
	return video_layer() != NULL;
}

#ifdef CONFIG_GAMES
#define HAVE_GAMES
#endif
#if defined(CONFIG_VIDEO_TESTPATTERN) || defined(HAVE_GAMES)
#define HAVE_VIDEO_SOURCES
#endif

#ifdef HAVE_VIDEO_SOURCES
#define FRAME_STATS_PERIOD_US 10000000

/* Frame times start again: a new source, or one back from the background */
static void frames_restart(bool new_source)
{
	frametimes_reset(&v.frame_times);
	v.frame_stats.last_us = 0;
	if (!new_source)
		return;
	v.frame_stats.over50 = v.frame_stats.over100 = 0;
	v.frame_stats.worst_us = 0;
	v.frame_stats.log_us = getenv("BARE_APP_FRAME_STATS") ? now_us() : 0;
}

/* BARE_APP_FRAME_STATS: the last stretch in the log */
static void frames_log(uint64_t now)
{
	static uint64_t t[FRAMETIMES_MAX];
	struct frametimes_summary s;
	unsigned n = frametimes_last(&v.frame_times, t, FRAMETIMES_MAX);

	frametimes_summarize(t, n, v.frame_stats.log_us, &s);
	v.frame_stats.log_us = now;
	if (!s.frames)
		return;
	LOGI(TAG, "frames: %u in %llu ms, %d.%d fps, median %u.%u ms, 99%% %u.%u ms, worst %u.%u ms, "
	     "%u hitches", s.frames, (unsigned long long)(s.span_us / 1000), s.fps_x10 / 10,
	     s.fps_x10 % 10, s.median_us / 1000, s.median_us / 100 % 10, s.p99_us / 1000,
	     s.p99_us / 100 % 10, s.worst_us / 1000, s.worst_us / 100 % 10, s.hitches);
}

static void frame_shown(void)
{
	uint64_t now = now_us();
	uint32_t d = v.frame_stats.last_us ? (uint32_t)MIN(now - v.frame_stats.last_us, UINT32_MAX) : 0;

	frametimes_add(&v.frame_times, now);
	v.frame_stats.last_us = now;
	v.frame_stats.over50 += d > 50000;
	v.frame_stats.over100 += d > 100000;
	v.frame_stats.worst_us = MAX(v.frame_stats.worst_us, d);
	if (v.frame_stats.log_us && now - v.frame_stats.log_us >= FRAME_STATS_PERIOD_US)
		frames_log(now);
}

static void frames_end_log(const char *what)
{
	LOGI(TAG, "%s: %u frames shown, %u over 50 ms, %u over 100 ms, worst %u ms", what,
	     v.video_frames, v.frame_stats.over50, v.frame_stats.over100,
	     v.frame_stats.worst_us / 1000);
}

/* Copy a whole frame of a source into the video layer and show it */
static void video_show(struct hal_layer *l, const struct hal_fb *src)
{
	struct hal_fb *dst = hal_layer_acquire(l);
	struct hal_rect all;
	int y;

	for (y = 0; y < dst->height && y < src->height; y++)
		memcpy((uint8_t *)dst->pixels + (size_t)y * dst->stride,
		       (uint8_t *)src->pixels + (size_t)y * src->stride,
		       (size_t)(dst->width < src->width ? dst->width : src->width) * 4);
	all = (struct hal_rect){ 0, 0, dst->width - 1, dst->height - 1 };
	hal_layer_damage(l, &all);
	hal_layer_submit(l);
	v.video_frames++;
	frame_shown();
}

static void video_clear(struct hal_layer *l)
{
	struct hal_fb *fb = hal_layer_acquire(l);
	struct hal_rect all;

	memset(fb->pixels, 0, (size_t)fb->stride * fb->height);
	all = (struct hal_rect){ 0, 0, fb->width - 1, fb->height - 1 };
	hal_layer_damage(l, &all);
	hal_layer_submit(l);
}
#endif

#ifdef CONFIG_VIDEO_TESTPATTERN
/* A new frame from the producer thread: copy it into the video layer */
static void on_video_frame(int fd, short revents, void *ud)
{
	struct hal_layer *l = video_layer();
	struct hal_fb *src;

	(void)fd;
	(void)revents;
	(void)ud;
	mailbox_ack(v.video_mb);
	src = mailbox_take(v.video_mb);
	if (src && l)
		video_show(l, src);
}
#endif

#ifdef HAVE_GAMES
/* A frame from the game, or the game has ended */
static void on_game(int fd, short revents, void *ud)
{
	struct hal_layer *l = video_layer();
	const struct hal_fb *fb;
	bool ended, minimized = v.video_minimized;
	int status;

	(void)revents;
	(void)ud;
	fb = game_proc_frame(v.video_game, &ended);
	if (fb && l && !minimized)
		video_show(l, fb);
	if (!ended)
		return;
	mainloop_remove_fd(v.ml, fd);
	status = game_proc_stop(v.video_game);
	v.video_game = NULL;
	v.video_minimized = false;
	frames_end_log(v.game.def->id);
	v.game = (struct game_ref){ 0 };
	if (l)
		video_clear(l);
	v.video_src = VIDEO_NONE;
	/* a minimised game has no screen that would listen */
	if (v.video_end && !minimized)
		v.video_end(status, v.video_end_ud);
}
#endif

/* A game that takes the gamepad now: the quit combo ends it */
static bool game_in_front(void)
{
#ifdef HAVE_GAMES
	return v.video_game && !v.video_minimized;
#else
	return false;
#endif
}

/* The quit combo held over a game: as if it was quit in its menu */
static void game_quit(void)
{
	view_video_end_cb cb = v.video_end;
	void *ud = v.video_end_ud;

	if (!game_in_front())
		return;
	LOGI(TAG, "quit combo: ending the game");
	view_video_set(VIDEO_NONE);
	if (cb)
		cb(0, ud);	/* exit status 0 */
}

/* F5 outside a game: back to the minimised one */
static void game_unminimize(void)
{
	if (view_video_minimized() != VIDEO_GAME)
		return;
	v.game_sel = v.game;
	gui_action_post(ACT_OPEN_SCREEN, SCREEN_GAME);
}

#ifdef HAVE_VIDEO_SOURCES
static void video_stop(struct hal_layer *l)
{
	if (v.video_src == VIDEO_NONE)
		return;
#ifdef CONFIG_VIDEO_TESTPATTERN
	if (v.video_mb) {
		testpattern_stop(v.video_tp);
		mainloop_remove_fd(v.ml, mailbox_fd(v.video_mb));
		LOGI(TAG, "video: %u frames shown, %u dropped", v.video_frames,
		     mailbox_dropped(v.video_mb));
		mailbox_free(v.video_mb);
		v.video_tp = NULL;
		v.video_mb = NULL;
	}
#endif
#ifdef HAVE_GAMES
	if (v.video_game) {
		mainloop_remove_fd(v.ml, game_proc_fd(v.video_game));
		game_proc_stop(v.video_game);
		v.video_game = NULL;
		v.video_minimized = false;
		frames_end_log(v.game.def->id);
		v.game = (struct game_ref){ 0 };
	}
#endif
	video_clear(l);	/* leave the layer black */
}
#endif

void view_video_input(const struct gp_event *ev)
{
#ifdef HAVE_GAMES
	if (v.video_game)
		game_proc_input(v.video_game, ev);
#else
	(void)ev;
#endif
}

void view_video_minimize(void)
{
#ifdef HAVE_GAMES
	if (!game_in_front())
		return;
	game_proc_pause(v.video_game);
	v.video_minimized = true;
	LOGI(TAG, "%s minimised", v.game.def->id);
#endif
}

enum gp_button view_game_minimize_button(void)
{
#ifdef HAVE_GAMES
	return game_minimize_button(v.game, v.pf);
#else
	return GP_F5;
#endif
}

void view_game_select(struct game_ref g)
{
	v.game_sel = g;
}

struct game_ref view_game_selected(void)
{
	return v.game_sel;
}

struct game_ref view_game(void)
{
	return v.game;
}

const struct platform *view_platform(void)
{
	return v.pf;
}

enum video_source view_video_shown(void)
{
	return view_video_minimized() == VIDEO_NONE ? v.video_src : VIDEO_NONE;
}

enum video_source view_video_minimized(void)
{
#ifdef HAVE_GAMES
	return v.video_minimized ? v.video_src : VIDEO_NONE;
#else
	return VIDEO_NONE;
#endif
}

unsigned view_video_frames(void)
{
	return v.video_frames;
}

unsigned view_video_frame_times(uint64_t *out, unsigned max)
{
	return frametimes_last(&v.frame_times, out, max);
}

void view_video_on_end(view_video_end_cb cb, void *ud)
{
	v.video_end = cb;
	v.video_end_ud = ud;
}

int view_video_set(enum video_source src)
{
	struct hal_layer *l = video_layer();
	struct hal_fb *fb;

	if (!l) {
		errno = ENODEV;
		return src == VIDEO_NONE ? 0 : -1;
	}
	if (src == v.video_src && (src != VIDEO_GAME || game_ref_eq(v.game, v.game_sel))) {
#ifdef HAVE_GAMES
		if (v.video_minimized) {
			/* its last frame is still in the layer, the count goes on */
			game_proc_resume(v.video_game);
			v.video_minimized = false;
			frames_restart(false);	/* the time away is no hitch */
			LOGI(TAG, "%s back", v.game.def->id);
		}
#endif
		return 0;
	}
#ifdef HAVE_VIDEO_SOURCES
	video_stop(l);
#endif
	v.video_src = VIDEO_NONE;
	v.video_frames = 0;
#ifdef HAVE_VIDEO_SOURCES
	frames_restart(true);
#endif
	switch (src) {
	case VIDEO_NONE:
		break;
#ifdef CONFIG_VIDEO_TESTPATTERN
	case VIDEO_TEST_PATTERN:
		fb = hal_layer_acquire(l);
		v.video_mb = mailbox_new(fb->width, fb->height);
		if (!v.video_mb)
			return -1;
		mainloop_add_fd(v.ml, mailbox_fd(v.video_mb), POLLIN, on_video_frame, NULL);
		v.video_tp = testpattern_start(v.video_mb, 30);
		break;
#endif
#ifdef HAVE_GAMES
	case VIDEO_GAME: {
		struct game_env env = { .pf = v.pf, .pcm = v.pf->game_pcm, .lang = i18n_lang() };

		if (!v.game_sel.def) {
			errno = EINVAL;
			return -1;
		}
		fb = hal_layer_acquire(l);
		env.width = fb->width;
		env.height = fb->height;
		v.video_game = v.game_sel.def->start(v.game_sel.entry, &env);
		if (!v.video_game)
			return -1;
		v.game = v.game_sel;
		mainloop_add_fd(v.ml, game_proc_fd(v.video_game), POLLIN, on_game, NULL);
		LOGI(TAG, "game %s, entry %d", v.game.def->id, v.game.entry);
		break;
	}
#endif
	default:
		(void)fb;
		errno = ENOTSUP;	/* not in this build */
		return -1;
	}
	v.video_src = src;
	LOGI(TAG, "video source %d", src);
	return 0;
}

/* ---- loop ------------------------------------------------------------- */

static void on_ctx(int fd, short revents, void *ud)
{
	(void)fd;
	(void)revents;
	(void)ud;
	ctx_ack();	/* the snapshot is taken in prepare(), once per iteration */
}

/*
 * Screenshot of the composed layers as PPM: on SIGUSR1 (to BARE_APP_SHOT or
 * /tmp/bare_app.ppm), and on exit if BARE_APP_SHOT is set, taken before the
 * UI is torn down.
 */
static void snapshot(bool on_request)
{
#ifdef CONFIG_SNAPSHOT
	const char *path = getenv("BARE_APP_SHOT");

	if (!path || !*path) {
		if (!on_request)
			return;
		path = "/tmp/bare_app.ppm";
	}
	if (v.display->ops->snapshot)
		v.display->ops->snapshot(v.display, path);
	else
		LOGW(TAG, "display \"%s\" cannot take snapshots", v.display->ops->name);
#else
	(void)on_request;
#endif
}

static void on_notify(int fd, short revents, void *ud)
{
	struct gui_notify n;

	(void)fd;
	(void)revents;
	(void)ud;
	notify_ack();
	while (notify_pop(&n)) {
		if (n.kind == NOTIFY_SNAPSHOT) {
			snapshot(true);
		} else if (n.kind == NOTIFY_RESUMED) {
			v.asleep = false;
			v.power_quiet_until = now_ms() + POWER_QUIET_MS;
			screen_on();	/* also if the device could not sleep */
		} else if (v.be->on_notify) {
			v.be->on_notify(&n);
		}
	}
}

static int prepare(void *ud)
{
	uint32_t changed;

	(void)ud;
	changed = ctx_snapshot(&v.snap);
	if (changed)
		dispatch(changed);
	return v.be->process ? v.be->process() : -1;
}

static void close_devices(void)
{
	int i;

	for (i = 0; i < PLATFORM_MAX_INPUTS; i++)
		if (v.inputs[i]) {
			((const struct hal_input_ops *)v.pf->inputs[i].ops)->close(v.inputs[i]);
			v.inputs[i] = NULL;
		}
	if (v.display) {
		v.display->ops->close(v.display);
		v.display = NULL;
	}
}

static int open_devices(void)
{
	int i, n = 0;

	for (i = 0; i < PLATFORM_MAX_DISPLAYS && !v.display; i++) {
		const struct hal_display_ops *dops = v.pf->displays[i].ops;

		if (!dops)
			continue;
		v.display = dops->open(v.pf->displays[i].params, v.ml);
		if (!v.display)
			LOGW(TAG, "display \"%s\" did not open", dops->name);
		else
			LOGI(TAG, "display \"%s\"", dops->name);
	}
	if (!v.display) {
		LOGE(TAG, "no display");
		return -1;
	}
	for (i = 0; i < PLATFORM_MAX_INPUTS; i++) {
		const struct hal_input_ops *iops = v.pf->inputs[i].ops;

		if (!iops)
			continue;
		v.inputs[i] = iops->open(v.pf->inputs[i].params, v.ml);
		n += v.inputs[i] != NULL;
	}
	if (!n)
		LOGW(TAG, "no input device opened");
	return 0;
}

/* Next to the settings file: bare_app_sticks.conf (the input test writes it) */
static void load_stick_calibration(void)
{
#ifdef CONFIG_SETTINGS_FILE
	const char *slash = v.pf->config_path ? strrchr(v.pf->config_path, '/') : NULL;
	char path[256];

	if (!slash)
		return;
	snprintf(path, sizeof(path), "%.*s/bare_app_sticks.conf",
		 (int)(slash - v.pf->config_path), v.pf->config_path);
	input_load_calibration(path);
#endif
}

static void *view_thread(void *arg)
{
	(void)arg;
	v.ml = mainloop_new("view");
	if (!v.ml) {
		v.status = -1;
		sem_post(&v.ready);
		return NULL;
	}
	input_init(v.ml, NULL, on_gamepad, NULL);
	load_stick_calibration();
	v.status = open_devices();
	if (!v.status) {
		ctx_snapshot(&v.snap);
		v.status = v.be->init(v.display, v.ml);
		if (v.status)
			LOGE(TAG, "backend \"%s\" failed to start", v.be->name);
	}
	if (!v.status) {
		mainloop_add_fd(v.ml, ctx_fd(), POLLIN, on_ctx, NULL);
		mainloop_add_fd(v.ml, notify_fd(), POLLIN, on_notify, NULL);
		mainloop_set_prepare(v.ml, prepare, NULL);
	}
	sem_post(&v.ready);
	if (!v.status) {
		LOGI(TAG, "running, backend \"%s\"", v.be->name);
		mainloop_run(v.ml);
		snapshot(false);
		v.be->deinit();
		view_video_set(VIDEO_NONE);
	}
	close_devices();
	input_deinit();
	mainloop_free(v.ml);
	v.ml = NULL;
	return NULL;
}

int view_start(const struct view_backend_ops *be, const struct platform *pf)
{
	int err;

	memset(&v, 0, sizeof(v));
	v.be = be;
	v.pf = pf;
	sem_init(&v.ready, 0, 0);
	err = pthread_create(&v.thread, NULL, view_thread, NULL);
	if (err) {
		LOGE(TAG, "pthread_create: %s", strerror(err));
		return -1;
	}
	while (sem_wait(&v.ready) < 0 && errno == EINTR)
		;
	if (v.status) {
		pthread_join(v.thread, NULL);
		return -1;
	}
	return 0;
}

void view_stop(void)
{
	if (!v.ml)
		return;
	mainloop_quit(v.ml);
	pthread_join(v.thread, NULL);
	sem_destroy(&v.ready);
}
