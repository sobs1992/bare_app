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
#include "gui/ctx.h"
#include "gui/notify.h"

struct hal_display;
struct mainloop;
struct platform;

enum ui_key {
	UI_KEY_UP, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT,
	UI_KEY_ACCEPT,		/* A */
	UI_KEY_BACK,		/* B */
	UI_KEY_PREV, UI_KEY_NEXT,	/* L1 / R1 */
	UI_KEY_MENU,		/* Start */
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
 * grabbed no UI keys are produced; the quit combo still works. Release
 * only drops the grab if 'ud' still owns it: a new screen may already
 * have grabbed before the old one is deleted. */
typedef void (*view_grab_cb)(const struct gp_event *ev, void *ud);
void view_input_grab(view_grab_cb cb, void *ud);
void view_input_release(void *ud);

const char *ui_key_name(enum ui_key key);
