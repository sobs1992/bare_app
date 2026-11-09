/*
 * View core (toolkit independent). Runs the view thread: opens display
 * and input drivers, takes context snapshots, calls subscriptions, turns
 * the gamepad into UI keys and drives one backend (LVGL, headless...).
 *
 * Everything here is called in the view thread only, except
 * view_start()/view_stop().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/input.h"
#include "games/game.h"
#include "gui/ctx.h"
#include "gui/notify.h"
#include "hal/platform.h"

struct hal_display;
struct mainloop;
struct platform;

enum ui_key {
	UI_KEY_UP, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT,
	UI_KEY_ACCEPT,		/* A */
	UI_KEY_BACK,		/* B */
	UI_KEY_PREV, UI_KEY_NEXT,	/* L1 / R1 */
	UI_KEY_MENU,		/* Start */
	UI_KEY_ALT,		/* X: second action on the focused item */
	UI_KEY_COUNT
};

struct view_backend_ops {
	const char *name;
	/* display may be NULL for backends that do not draw */
	int (*init)(struct hal_display *display, struct mainloop *ml);
	/* returns true if the key was used; an unused BACK press closes the screen */
	bool (*on_key)(enum ui_key key, bool pressed, bool repeat);
	void (*on_notify)(const struct gui_notify *n);
	/* draw what changed; returns ms until the next call is due, -1 none */
	int (*process)(void);
	void (*deinit)(void);
};

int view_start(const struct view_backend_ops *be, const struct platform *pf);
void view_stop(void);

typedef void (*view_cb)(void *self, const struct gui_ctx *snap, uint32_t changed);

/* cb is called right away with the current snapshot (changed = CTX_ALL) */
int view_subscribe(uint32_t sections, view_cb cb, void *self);
void view_unsubscribe(int id);
const struct gui_ctx *view_ctx(void);

/* Raw gamepad events to one owner (e.g. an input test screen). While
 * grabbed no UI keys are produced; the quit combo still works (over a
 * running game it ends the game instead of the application). Release
 * only drops the grab if 'ud' still owns it: a new screen may already
 * have grabbed before the old one is deleted. */
typedef void (*view_grab_cb)(const struct gp_event *ev, void *ud);
void view_input_grab(view_grab_cb cb, void *ud);
void view_input_release(void *ud);

const char *ui_key_name(enum ui_key key);

/* Volume keys (and what a game uses instead) change the volume this much */
#define VIEW_VOLUME_STEP 5	/* percent */

/*
 * Video layer under the UI (hardware plane). Screens that show video make
 * their background transparent and pick a source; only one source runs.
 */
enum video_source {
	VIDEO_NONE,
	VIDEO_TEST_PATTERN,
	VIDEO_GAME,		/* view_game_selected(): in its own process, takes the gamepad */
};

bool view_video_available(void);
/* -1 if there is no video layer or the source did not start (errno) */
int view_video_set(enum video_source src);

/* Gamepad events for a source that takes them (a game) */
void view_video_input(const struct gp_event *ev);

/*
 * A game can be minimised: its process stops in the background and keeps
 * its last frame in the layer, under the UI. F5 (or the game's minimise
 * button) on any screen without a grab opens its screen again (with the
 * system monitor in the build: a short press, on release), and
 * view_video_set() of the same source continues it; any other source ends
 * it.
 */
void view_video_minimize(void);
/* The button that minimises the game in the layer (game_def.minimize_for) */
enum gp_button view_game_minimize_button(void);
/* The minimised game, VIDEO_NONE if there is none */
enum video_source view_video_minimized(void);
/* The source in the layer now, VIDEO_NONE also while it is minimised */
enum video_source view_video_shown(void);

/*
 * The game VIDEO_GAME starts (games/game.h): picked in the menu before its
 * screen opens. view_video_set(VIDEO_GAME) of another game than the one in
 * the layer ends that one.
 */
void view_game_select(struct game_ref g);
struct game_ref view_game_selected(void);
/* The game in the layer, in front or minimised; def NULL if none */
struct game_ref view_game(void);
/* What this platform starts games with: the platform, its PCM device */
const struct platform *view_platform(void);

/* Frames shown since the source was set (a game that is still loading: 0) */
unsigned view_video_frames(void);
/* When the latest of them were shown (now_us()), oldest first, up to max
 * (core/frametimes.h); returns how many. Starts again when a minimised
 * game comes back. */
unsigned view_video_frame_times(uint64_t *out, unsigned max);
/* Whether a gap in those frames means the source stalled: the source says
 * when a frame did not change (GAME_FRAME_SAME). Otherwise a long gap may
 * just be a still picture. */
bool view_video_paced(void);

/* Called when the running source ends by itself (the game quit), with its
 * waitpid() status; not when view_video_set() stops it. One listener. */
typedef void (*view_video_end_cb)(int status, void *ud);
void view_video_on_end(view_video_end_cb cb, void *ud);
