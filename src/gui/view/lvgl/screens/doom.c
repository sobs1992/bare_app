/*
 * Doom (GAME_DOOM): the game draws into the video layer under a
 * transparent screen and gets the gamepad. F5 or Quit in Doom's menu
 * comes back here (the quit combo still ends the application). When the
 * game cannot start or ends with an error the screen says why.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../widget.h"
#include "games/doom.h"
#include "gui/action.h"

struct doom_screen {
	lv_obj_t *scr;
};

static void show_message(struct doom_screen *d, const char *text)
{
	lv_obj_t *body, *label;

	lv_obj_set_style_bg_opa(d->scr, LV_OPA_COVER, 0);
	statusbar_create(d->scr, LV_SYMBOL_PLAY, tr(S_DOOM));
	body = screen_body(d->scr);
	label = lv_label_create(body);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text_fmt(label, LV_SYMBOL_WARNING "  %s", text);
}

static void on_input(const struct gp_event *ev, void *ud)
{
	(void)ud;
	if (ev->type == GP_EV_BUTTON && ev->code == GP_F5) {
		if (ev->pressed && !ev->repeat)
			gui_action_post(ACT_BACK, 0);	/* deinit stops the game */
		return;
	}
	view_video_input(ev);
}

static void on_end(int status, void *ud)
{
	struct doom_screen *d = ud;
	char text[160];

	view_input_release(d);
	view_video_on_end(NULL, NULL);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		gui_action_post(ACT_BACK, 0);	/* Quit in Doom's menu */
		return;
	}
	if (WIFSIGNALED(status))
		snprintf(text, sizeof(text), tr(S_DOOM_SIGNAL_FMT), WTERMSIG(status));
	else
		snprintf(text, sizeof(text), tr(S_DOOM_EXIT_FMT), WEXITSTATUS(status));
	show_message(d, text);
}

static void *doom_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct doom_screen *d = calloc(1, sizeof(*d));
	char text[256];

	(void)parent;
	if (!d)
		return NULL;
	d->scr = lv_obj_create(NULL);
	*root = d->scr;
	if (view_video_set(VIDEO_DOOM) == 0) {
		lv_obj_set_style_bg_opa(d->scr, LV_OPA_TRANSP, 0);
		view_input_grab(on_input, d);
		view_video_on_end(on_end, d);
		ui_toast(tr(S_DOOM_HINT));
	} else if (!view_video_available()) {
		show_message(d, tr(S_LAYERS_NO_VIDEO));
	} else if (access(doom_wad(), R_OK) < 0) {
		snprintf(text, sizeof(text), tr(S_DOOM_NO_WAD_FMT), doom_wad());
		show_message(d, text);
	} else {
		show_message(d, tr(S_DOOM_FAILED));
	}
	return d;
}

static void doom_deinit(void *self)
{
	view_input_release(self);
	view_video_on_end(NULL, NULL);
	view_video_set(VIDEO_NONE);
	free(self);
}

const struct widget_ops doom_screen = {
	.name = "doom",
	.deps = 0,
	.init = doom_init,
	.deinit = doom_deinit,
};
