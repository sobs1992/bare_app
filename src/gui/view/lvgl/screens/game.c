/*
 * Games (GAME_DOOM, GAME_HOMM3): the game draws into the video layer under
 * a transparent screen and gets the gamepad. "Loading..." stays until its
 * first frame, then a hint shows the controls. Games that do not need the
 * triggers leave them for the volume (L2 down, R2 up). F5 minimises the
 * game: it stops in the background, and F5 or its menu entry brings it
 * back. Holding the quit combo (F6) or Quit in the game ends it and comes
 * back here. When the game cannot start or ends with an error the screen
 * says why.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../widget.h"
#include "games/doom.h"
#include "games/homm3.h"
#include "gui/action.h"

#define WAIT_MS 100

struct game_desc {
	enum video_source src;
	enum str_id title, hint;
	bool trigger_volume;	/* L2, R2: volume instead of the game */
	/* Why the game cannot start (missing data) into buf; false if nothing is missing */
	bool (*missing)(char *buf, size_t size);
};

struct game_screen {
	lv_obj_t *scr;
	const struct game_desc *desc;
	lv_obj_t *loading;
	lv_timer_t *wait;	/* for the first frame */
};

static void stop_waiting(struct game_screen *g)
{
	if (g->wait)
		lv_timer_delete(g->wait);
	g->wait = NULL;
	if (g->loading)
		lv_obj_delete(g->loading);
	g->loading = NULL;
}

static void show_message(struct game_screen *g, const char *text)
{
	lv_obj_t *body, *label;

	stop_waiting(g);
	lv_obj_set_style_bg_opa(g->scr, LV_OPA_COVER, 0);
	statusbar_create(g->scr, LV_SYMBOL_PLAY, tr(g->desc->title));
	body = screen_body(g->scr);
	label = lv_label_create(body);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text_fmt(label, LV_SYMBOL_WARNING "  %s", text);
}

static void on_wait(lv_timer_t *t)
{
	struct game_screen *g = lv_timer_get_user_data(t);

	if (!view_video_frames())
		return;
	stop_waiting(g);
	ui_toast(tr(g->desc->hint));
}

static void on_input(const struct gp_event *ev, void *ud)
{
	struct game_screen *g = ud;

	if (ev->type == GP_EV_BUTTON && ev->code == GP_F5) {
		if (ev->pressed && !ev->repeat) {
			view_video_minimize();
			gui_action_post(ACT_BACK, 0);	/* deinit leaves it running */
		}
		return;
	}
	if (ev->type == GP_EV_BUTTON && g->desc->trigger_volume &&
	    (ev->code == GP_L2 || ev->code == GP_R2)) {
		if (ev->pressed)
			gui_action_post(ACT_VOLUME_STEP, ev->code == GP_R2 ? VIEW_VOLUME_STEP
									   : -VIEW_VOLUME_STEP);
		return;
	}
	view_video_input(ev);
}

static void on_end(int status, void *ud)
{
	struct game_screen *g = ud;
	char text[160];

	view_input_release(g);
	view_video_on_end(NULL, NULL);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		gui_action_post(ACT_BACK, 0);	/* Quit in the game's menu */
		return;
	}
	if (WIFSIGNALED(status))
		snprintf(text, sizeof(text), tr(S_GAME_SIGNAL_FMT), WTERMSIG(status));
	else
		snprintf(text, sizeof(text), tr(S_GAME_EXIT_FMT), WEXITSTATUS(status));
	show_message(g, text);
}

static void *game_init(const struct game_desc *desc, lv_obj_t **root)
{
	struct game_screen *g = calloc(1, sizeof(*g));
	char text[256];

	if (!g)
		return NULL;
	g->desc = desc;
	g->scr = lv_obj_create(NULL);
	*root = g->scr;
	if (view_video_set(desc->src) == 0) {	/* or back from minimised */
		lv_obj_set_style_bg_opa(g->scr, LV_OPA_TRANSP, 0);
		view_input_grab(on_input, g);
		view_video_on_end(on_end, g);
		if (view_video_frames()) {
			ui_toast(tr(desc->hint));
			return g;
		}
		g->loading = lv_label_create(g->scr);	/* over the black video layer */
		lv_obj_set_style_text_color(g->loading, lv_color_white(), 0);
		lv_obj_set_style_text_font(g->loading, UI_FONT_MID, 0);
		lv_label_set_text(g->loading, tr(S_GAME_LOADING));
		lv_obj_center(g->loading);
		g->wait = lv_timer_create(on_wait, WAIT_MS, g);
	} else if (!view_video_available()) {
		show_message(g, tr(S_LAYERS_NO_VIDEO));
	} else if (desc->missing(text, sizeof(text))) {
		show_message(g, text);
	} else {
		show_message(g, tr(S_GAME_FAILED));
	}
	return g;
}

static void game_deinit(void *self)
{
	struct game_screen *g = self;

	if (g->wait)
		lv_timer_delete(g->wait);
	view_input_release(g);
	view_video_on_end(NULL, NULL);
	if (view_video_minimized() == VIDEO_NONE)
		view_video_set(VIDEO_NONE);
	free(g);
}

#ifdef CONFIG_GAME_DOOM
static bool doom_missing(char *buf, size_t size)
{
	if (access(doom_wad(), R_OK) == 0)
		return false;
	snprintf(buf, size, tr(S_DOOM_NO_WAD_FMT), doom_wad());
	return true;
}

static const struct game_desc doom_desc = { VIDEO_DOOM, S_DOOM, S_DOOM_HINT, true,
					    doom_missing };

static void *doom_init(lv_obj_t *parent, lv_obj_t **root)
{
	(void)parent;
	return game_init(&doom_desc, root);
}

const struct widget_ops doom_screen = {
	.name = "doom",
	.init = doom_init,
	.deinit = game_deinit,
};
#endif

#ifdef CONFIG_GAME_HOMM3
static bool homm3_missing_text(char *buf, size_t size)
{
	const char *missing = homm3_missing();

	if (!missing)
		return false;
	snprintf(buf, size, tr(S_GAME_MISSING_FMT), missing);
	return true;
}

static const struct game_desc homm3_desc = { VIDEO_HOMM3, S_HOMM3, S_HOMM3_HINT, true,
					     homm3_missing_text };

static void *homm3_init(lv_obj_t *parent, lv_obj_t **root)
{
	(void)parent;
	return game_init(&homm3_desc, root);
}

const struct widget_ops homm3_screen = {
	.name = "homm3",
	.init = homm3_init,
	.deinit = game_deinit,
};
#endif
