/*
 * The screen of every game (games/game.h), the one view_game_selected()
 * names when it opens: the game draws into the video layer under
 * a transparent screen and gets the gamepad. "Loading..." stays until its
 * first frame, then a hint shows the controls if the setting says so
 * (ctx settings.game_hint, off by default). Games that do not need the
 * triggers leave them for the volume (L2 down, R2 up). F5 (Heroes III
 * with the GO Ultra's keys: F1) minimises the game: it stops in the
 * background, and the same button or its menu entry brings it back. Holding the quit combo (F6) or Quit in the game ends it and comes
 * back here. When the game cannot start or ends with an error the screen
 * says why.
 *
 * A low battery is said over the game (CTX_POWER): a warning at the top
 * for a few seconds when the game starts or comes back with the battery at
 * 15 % or less, and again each time it goes down to 10, 5, 3 and 1 %; at
 * 5 % and less the battery pill (widgets/battery.c) stays where the status
 * bar has it as well (over the system monitor's frame graph, if that is
 * shown). A charger
 * puts them away and starts over. The application's own "battery low"
 * notice is left out while a game is in front.
 *
 * The game's own controls (ctx settings.controls, the controls screen)
 * turn each button into the one the game gets, after the buttons bare_app
 * keeps (the minimise button, the triggers for the volume). Directions of
 * the left stick are not buttons to map, but they follow its axes when the
 * controls invert them, as the axes the game gets do.
 *
 * The motion sensor (GP_EV_MOTION, core/motion.h) moves the game's mouse
 * while its controls (or the game's default, game_gyro()) say so: the
 * degrees the device turned times the game's counts per degree times the
 * sensitivity, the parts of a count kept for the next move.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../widget.h"
#include "games/game.h"
#include "gui/action.h"

#define WAIT_MS 100
#define BATTERY_WARN_MS 6000
#define BATTERY_CRITICAL 5	/* percent: the corner's battery from here */

/* The warning comes again at each of these, going down */
static const int battery_levels[] = { 15, 10, 5, 3, 1 };
#define BATTERY_LEVELS ((int)(sizeof(battery_levels) / sizeof(battery_levels[0])))

struct game_screen {
	lv_obj_t *scr;
	struct game_ref game;
	char key[GAME_KEY_SIZE];
	lv_obj_t *loading;
	lv_timer_t *wait;	/* for the first frame */
	int battery_next;	/* battery_levels[] index not warned at yet */
	lv_obj_t *battery_warn, *battery_corner;	/* on the top layer */
	lv_timer_t *battery_timer;	/* takes the warning away */
	bool gyro_default;	/* game_gyro() */
	float gyro_counts;
	float gyro_rest[2];	/* parts of a count not sent yet */
};

static bool game_shown;		/* a game screen is up: the battery is said here */

bool game_screen_battery(void)
{
	return game_shown;
}

/* ---- the battery ---------------------------------------------------------- */

static void battery_warn_hide(struct game_screen *g)
{
	if (g->battery_timer)
		lv_timer_delete(g->battery_timer);
	g->battery_timer = NULL;
	if (g->battery_warn)
		lv_obj_delete(g->battery_warn);
	g->battery_warn = NULL;
}

static void on_battery_timer(lv_timer_t *t)
{
	struct game_screen *g = lv_timer_get_user_data(t);

	battery_warn_hide(g);
}

static lv_obj_t *battery_box(lv_color_t bg)
{
	lv_obj_t *box = lv_label_create(lv_layer_top());

	lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
	lv_obj_set_style_bg_color(box, bg, 0);
	lv_obj_set_style_radius(box, ui_px(8), 0);
	lv_obj_set_style_text_color(box, lv_color_white(), 0);
	lv_obj_set_style_text_font(box, UI_FONT_MID, 0);
	return box;
}

static void battery_warn_show(struct game_screen *g, int capacity)
{
	char text[160];
	int n;

	battery_warn_hide(g);
	n = lv_snprintf(text, sizeof(text), "%s  ", LV_SYMBOL_BATTERY_EMPTY);
	n += lv_snprintf(text + n, sizeof(text) - n, tr(S_BATTERY_LOW_FMT), capacity);
	lv_snprintf(text + n, sizeof(text) - n, "\n%s", tr(S_BATTERY_PLUG_IN));
	g->battery_warn = battery_box(lv_color_hex(0xb3261e));
	lv_obj_set_style_pad_hor(g->battery_warn, ui_px(16), 0);
	lv_obj_set_style_pad_ver(g->battery_warn, ui_px(10), 0);
	lv_obj_set_style_text_align(g->battery_warn, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(g->battery_warn, text);
	lv_obj_align(g->battery_warn, LV_ALIGN_TOP_MID, 0, ui_px(12));
	g->battery_timer = lv_timer_create(on_battery_timer, BATTERY_WARN_MS, g);
	lv_timer_set_repeat_count(g->battery_timer, 1);
	lv_timer_set_auto_delete(g->battery_timer, false);
}

static void battery_corner(struct game_screen *g, int capacity)
{
	if (capacity < 0) {
		if (g->battery_corner)
			lv_obj_delete(g->battery_corner);
		g->battery_corner = NULL;
		return;
	}
	if (!g->battery_corner) {
		/* the status bar's pill where the status bar has it (its right
		 * end at the margin, in the middle of the header), on a dark
		 * backing around it, apart from the game */
		int32_t pad = ui_px(5);

		g->battery_corner = lv_obj_create(lv_layer_top());
		lv_obj_remove_style_all(g->battery_corner);
		lv_obj_set_scrollable(g->battery_corner, false);
		lv_obj_set_size(g->battery_corner, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(g->battery_corner, LV_OPA_50, 0);
		lv_obj_set_style_bg_color(g->battery_corner, lv_color_black(), 0);
		lv_obj_set_style_radius(g->battery_corner, ui_px(8), 0);
		lv_obj_set_style_pad_all(g->battery_corner, pad, 0);
		battery_pill_create(g->battery_corner);
		lv_obj_update_layout(g->battery_corner);
		lv_obj_align(g->battery_corner, LV_ALIGN_TOP_RIGHT, pad - ui_margin(),
			     (ui_header_h() - lv_obj_get_height(g->battery_corner)) / 2);
	}
	battery_pill_set(lv_obj_get_child(g->battery_corner, 0), capacity, false);
}

/* The battery as the context says: a warning at each level passed */
static void battery_follow(struct game_screen *g, const struct gui_ctx *c)
{
	int cap = c ? c->power.capacity : -1;
	bool low = c && c->power.available && c->power.present && !c->power.online && cap >= 0;
	bool warn = false;

	if (!low) {
		/* a charger (or no battery): from the start again */
		g->battery_next = 0;
		battery_warn_hide(g);
		battery_corner(g, -1);
		return;
	}
	while (g->battery_next < BATTERY_LEVELS && cap <= battery_levels[g->battery_next]) {
		g->battery_next++;
		warn = true;
	}
	if (warn)
		battery_warn_show(g, cap);
	battery_corner(g, cap <= BATTERY_CRITICAL ? cap : -1);
}

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
	screen_console_style(g->scr);
	console_header(g->scr, LV_SYMBOL_PLAY, game_title(g->game));
	body = screen_body(g->scr);
	label = lv_label_create(body);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text_fmt(label, LV_SYMBOL_WARNING "  %s", text);
}

static void show_hint(struct game_screen *g)
{
	const struct gui_ctx *c = view_ctx();

	if (c && c->settings.game_hint)
		ui_toast(game_hint(g->game, view_platform()));
}

static void on_wait(lv_timer_t *t)
{
	struct game_screen *g = lv_timer_get_user_data(t);

	if (!view_video_frames())
		return;
	stop_waiting(g);
	show_hint(g);
}

/* The device turned: the game's mouse moves */
static void gyro_move(struct game_screen *g, const struct gp_event *ev, const struct gp_remap *m)
{
	struct gp_event mv = { .type = GP_EV_MOUSE_MOVE };
	float counts, x, y;

	if (!gp_remap_gyro(m, g->gyro_default)) {
		g->gyro_rest[0] = g->gyro_rest[1] = 0;
		return;
	}
	counts = g->gyro_counts * gp_remap_gyro_sens(m);
	x = ev->mx * counts + g->gyro_rest[0];
	y = ev->my * counts + g->gyro_rest[1];
	mv.dx = (int)lrintf(x);
	mv.dy = (int)lrintf(y);
	g->gyro_rest[0] = x - mv.dx;
	g->gyro_rest[1] = y - mv.dy;
	if (mv.dx || mv.dy)
		view_video_input(&mv);
}

static void on_input(const struct gp_event *ev, void *ud)
{
	struct game_screen *g = ud;
	const struct gp_remap *m;
	struct gp_event mapped;

	if (ev->type == GP_EV_MOTION) {
		gyro_move(g, ev, ctx_controls(view_ctx(), g->key));
		return;
	}

	if (ev->type == GP_EV_BUTTON && ev->code == view_game_minimize_button()) {
		if (ev->pressed && !ev->repeat) {
			view_video_minimize();
			gui_action_post(ACT_BACK, 0);	/* deinit leaves it running */
		}
		return;
	}
	if (ev->type == GP_EV_BUTTON && game_trigger_volume(g->game) &&
	    (ev->code == GP_L2 || ev->code == GP_R2)) {
		if (ev->pressed)
			gui_action_post(ACT_VOLUME_STEP, ev->code == GP_R2 ? VIEW_VOLUME_STEP
									   : -VIEW_VOLUME_STEP);
		return;
	}
	m = ctx_controls(view_ctx(), g->key);
	if (m && ev->type == GP_EV_BUTTON) {
		mapped = *ev;
		mapped.code = ev->stick ? gp_remap_stick_dir(m, ev->code) : gp_remap_apply(m, ev->code);
		if (mapped.code != GP_NONE)
			view_video_input(&mapped);
		return;
	}
	if (m && ev->type == GP_EV_AXIS) {
		mapped = *ev;
		mapped.value = gp_remap_axis(m, ev->code, ev->value);
		view_video_input(&mapped);
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

static void *game_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct game_screen *g = calloc(1, sizeof(*g));
	char text[256];

	(void)parent;
	if (!g)
		return NULL;
	g->game = view_game_selected();
	game_key(g->game, g->key);
	g->gyro_default = game_gyro(g->game, &g->gyro_counts);
	g->scr = lv_obj_create(NULL);
	*root = g->scr;
	game_shown = true;
	if (!g->game.def) {
		show_message(g, tr(S_GAME_FAILED));
	} else if (view_video_set(VIDEO_GAME) == 0) {	/* or back from minimised */
		/* the home screen puts it first */
		gui_action_post_str(ACT_GAME_PLAYED, g->key);
		lv_obj_set_style_bg_opa(g->scr, LV_OPA_TRANSP, 0);
		view_input_grab(on_input, g);
		view_video_on_end(on_end, g);
		if (view_video_frames()) {
			show_hint(g);
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
	} else if (game_missing_text(g->game, text, sizeof(text))) {
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
	battery_warn_hide(g);
	battery_corner(g, -1);
	game_shown = false;
	view_input_release(g);
	view_video_on_end(NULL, NULL);
	if (view_video_minimized() == VIDEO_NONE)
		view_video_set(VIDEO_NONE);
	free(g);
}

static void game_update(void *self, const struct gui_ctx *ctx, uint32_t changed)
{
	struct game_screen *g = self;

	(void)changed;
	/* only over a game that runs, not over a message */
	if (view_video_frames() || g->loading)
		battery_follow(g, ctx);
}

const struct widget_ops game_screen = {
	.name = "game",
	.deps = CTX_POWER,
	.init = game_init,
	.update = game_update,
	.deinit = game_deinit,
};
