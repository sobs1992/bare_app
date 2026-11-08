/*
 * Home, laid out like a console's home menu: the games of games/game.h as
 * a row of big square tiles (their cover.png, or the title on a colour of
 * its own), the name of the focused one above it, a row of round buttons
 * for the other screens under it, and what A does at the bottom. Left and
 * Right go along a row, Up and Down between the rows (port_input_set_2d()).
 *
 * A minimised game has a pause mark; picking another entry that needs
 * the video layer (another game) or the games partition (the USB disk)
 * first asks whether to close it. A game whose files are missing is
 * dimmed: its screen says what is missing.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../widget.h"
#include "games/game.h"
#include "gui/action.h"

#define BUTTON_H 40		/* the dialog's */
#define BUTTON_GAP 8
#define DIALOG_W 400
#define MENU_MAX 64

#define TILE_MAX 240		/* tile side on a big screen */
#define TILE_SMALL 150		/* below it the small font on the tile */
#define TILE_MIN 96
#define TILE_GAP 18
#define TILE_RADIUS 12
#define FOCUS_W 4		/* the focused tile's or button's frame */
#define FOCUS_PAD 4
#define ROUND_GAP 22
#define FOOTER_H_MAX 40
#define COLOR_BG lv_color_hex(0x15181e)
#define COLOR_BG2 lv_color_hex(0x0d0f13)
#define COLOR_ROUND lv_color_hex(0x2a2f39)
#define COLOR_LINE lv_color_hex(0x2a2f39)
#define COLOR_ACCENT lv_color_hex(0x31c6ff)

struct entry {
	const char *symbol;
	enum str_id text;
	enum gui_action_type action;
	int arg;
	enum video_source video;	/* what the screen shows in the video layer */
	bool needs_games;		/* takes /games away: a minimised game must close */
	struct game_ref game;		/* a game: SCREEN_GAME plays it */
};

static const struct entry entries[] = {
#ifdef CONFIG_SCREEN_INPUT_TEST
	{ LV_SYMBOL_KEYBOARD, S_INPUT_TEST, ACT_OPEN_SCREEN, SCREEN_INPUT_TEST, VIDEO_NONE, false, { 0 } },
#endif
#ifdef CONFIG_SCREEN_USB_DISK
	{ LV_SYMBOL_USB, S_USB_DISK, ACT_OPEN_SCREEN, SCREEN_USB_DISK, VIDEO_NONE, true, { 0 } },
#endif
#ifdef CONFIG_SCREEN_SETTINGS
	{ LV_SYMBOL_SETTINGS, S_SETTINGS, ACT_OPEN_SCREEN, SCREEN_SETTINGS, VIDEO_NONE, false, { 0 } },
#endif
#ifdef CONFIG_SCREEN_LAYERS
	{ LV_SYMBOL_IMAGE, S_LAYERS, ACT_OPEN_SCREEN, SCREEN_LAYERS, VIDEO_TEST_PATTERN, false, { 0 } },
#endif
	{ LV_SYMBOL_POWER, S_QUIT, ACT_POWER_OFF, 0, VIDEO_NONE, false, { 0 } },
};

#define N_ENTRIES (sizeof(entries) / sizeof(entries[0]))

/* Every game's entries, then entries[] */
static struct entry menu[MENU_MAX];
static size_t n_menu, n_games;

static void build_menu(void)
{
	struct game_ref running = view_game();
	size_t i;

	n_menu = 0;
#ifdef CONFIG_GAMES
	for (const struct game_def *const *g = games; *g; g++) {
		/* not while one of it runs: its entry number must stay */
		int n = game_entries(*g, running.def != *g);

		for (int e = 0; e < n && n_menu < MENU_MAX - N_ENTRIES; e++)
			menu[n_menu++] = (struct entry){ LV_SYMBOL_PLAY, 0, ACT_OPEN_SCREEN, SCREEN_GAME,
							 VIDEO_GAME, false, { *g, e } };
	}
#else
	(void)running;
#endif
	n_games = n_menu;
	for (i = 0; i < N_ENTRIES; i++)
		menu[n_menu++] = entries[i];
}

static const char *entry_text(const struct entry *en)
{
	return en->game.def ? game_title(en->game) : tr(en->text);
}

static void start(const struct entry *en)
{
	if (en->game.def)
		view_game_select(en->game);
	gui_action_post(en->action, en->arg);
}

/* ---- "close the minimised game?" ---------------------------------------- */

/* Over the menu, with its own focus group */
struct dialog {
	lv_obj_t *backdrop;
	lv_group_t *group, *menu_group;
	const struct entry *picked;
};

/* The entry of the game in the layer */
static const struct entry *running_entry(void)
{
	struct game_ref g = view_game();
	size_t i;

	for (i = 0; g.def && i < n_menu; i++)
		if (game_ref_eq(menu[i].game, g))
			return &menu[i];
	return NULL;
}

static void dialog_close(struct dialog *d)
{
	port_input_set_group(d->menu_group);
	/* from an event of its own button: deleted after the event */
	lv_obj_delete_async(d->backdrop);	/* on_dialog_delete frees the rest */
}

static void on_dialog_delete(lv_event_t *e)
{
	struct dialog *d = lv_event_get_user_data(e);

	/* also when the menu is rebuilt under it: its new group is set by then */
	lv_group_delete(d->group);
	free(d);
}

static void on_cancel(lv_event_t *e)
{
	dialog_close(lv_event_get_user_data(e));
}

static void on_close_and_start(lv_event_t *e)
{
	struct dialog *d = lv_event_get_user_data(e);
	const struct entry *en = d->picked;

	dialog_close(d);
	view_video_set(VIDEO_NONE);	/* ends the minimised game */
	start(en);
}

static lv_obj_t *dialog_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
			       struct dialog *d)
{
	lv_obj_t *btn = lv_button_create(parent), *label;

	lv_obj_set_size(btn, LV_PCT(100), BUTTON_H);
	ui_focus_style(btn);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, d);
	lv_obj_add_event_cb(btn, on_cancel, LV_EVENT_CANCEL, d);	/* B */
	label = lv_label_create(btn);
	lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
	lv_label_set_text(label, text);
	lv_obj_center(label);
	return btn;
}

static void ask_close(lv_obj_t *scr, const struct entry *picked, const struct entry *running)
{
	struct dialog *d = calloc(1, sizeof(*d));
	lv_obj_t *box, *label;

	if (!d)
		return;
	d->picked = picked;
	d->menu_group = lv_group_get_default();
	d->group = lv_group_create();
	lv_group_set_wrap(d->group, true);
	lv_group_set_default(d->group);	/* the buttons join it */

	d->backdrop = lv_obj_create(scr);
	lv_obj_remove_style_all(d->backdrop);
	lv_obj_set_floating(d->backdrop, true);
	lv_obj_set_size(d->backdrop, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(d->backdrop, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(d->backdrop, LV_OPA_60, 0);
	lv_obj_add_event_cb(d->backdrop, on_dialog_delete, LV_EVENT_DELETE, d);

	box = lv_obj_create(d->backdrop);
	lv_obj_set_size(box, LV_MIN(DIALOG_W, lv_obj_get_width(scr) - 20), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(box, BUTTON_GAP, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_center(box);
	label = lv_label_create(box);
	lv_obj_set_width(label, LV_PCT(100));
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text_fmt(label, tr(S_GAME_MINIMIZED_FMT), entry_text(running),
			      entry_text(picked));
	dialog_button(box, tr(S_CANCEL), on_cancel, d);
	dialog_button(box, tr(S_CLOSE_AND_START), on_close_and_start, d);

	lv_group_set_default(d->menu_group);
	port_input_set_group(d->group);
	lv_obj_add_state(lv_group_get_focused(d->group), LV_STATE_FOCUS_KEY);
}

static void on_click(lv_event_t *e)
{
	const struct entry *en = lv_event_get_user_data(e);
	const struct entry *running = view_video_minimized() != VIDEO_NONE ? running_entry() : NULL;

	if (running && (en->video != VIDEO_NONE || en->needs_games) && en != running) {
		ask_close(lv_obj_get_screen(lv_event_get_target_obj(e)), en, running);
		return;
	}
	start(en);	/* the same game comes back */
}

/* ---- the menu ------------------------------------------------------------ */

struct home {
	lv_obj_t *scr, *title, *row, *rounds, *round_name, *action;
	lv_obj_t *tiles[MENU_MAX], *buttons[N_ENTRIES];
	const struct entry *tile_entry[MENU_MAX], *button_entry[N_ENTRIES];
	int n_tiles, n_buttons, last_tile, last_button;
	lv_group_t *group;
	int32_t tile;		/* side */
};

static struct home *home;	/* one home screen at a time */

/* A colour of its own for a game without a cover, from its title */
static lv_color_t title_color(const char *title)
{
	static const uint32_t palette[] = {
		0x2d5f9e, 0x7b3f9e, 0x9e3f5a, 0x2f7f5e, 0x9e6b2d, 0x3f709e, 0x5e3f9e, 0x7f2f2f,
	};
	uint32_t h = 2166136261u;

	for (; *title; title++)
		h = (h ^ (uint8_t)*title) * 16777619u;
	return lv_color_hex(palette[h % (sizeof(palette) / sizeof(palette[0]))]);
}

static int index_of(lv_obj_t *const *objs, int n, lv_obj_t *obj)
{
	for (int i = 0; i < n; i++)
		if (objs[i] == obj)
			return i;
	return -1;
}

static void set_action(const struct entry *en)
{
	const struct entry *running = view_video_minimized() != VIDEO_NONE ? running_entry() : NULL;
	enum str_id what = en->game.def ? (en == running ? S_HOME_RESUME : S_HOME_START)
			   : en->action == ACT_POWER_OFF ? S_QUIT : S_HOME_OPEN;

	lv_label_set_text(home->action, tr(what));
}

static void focus_changed(lv_obj_t *obj, const struct entry *en)
{
	int i;

	if ((i = index_of(home->tiles, home->n_tiles, obj)) >= 0) {
		home->last_tile = i;
		lv_label_set_text(home->title, entry_text(en));
		lv_obj_set_hidden(home->round_name, true);
		lv_obj_scroll_to_view(obj, LV_ANIM_ON);
	} else if ((i = index_of(home->buttons, home->n_buttons, obj)) >= 0) {
		home->last_button = i;
		lv_label_set_text(home->round_name, entry_text(en));
		lv_obj_set_hidden(home->round_name, false);
	}
	set_action(en);
}

static void on_focused(lv_event_t *e)
{
	if (home)
		focus_changed(lv_event_get_target_obj(e), lv_event_get_user_data(e));
}

/* Left, Right along a row, Up, Down between them; the other keys as usual */
static void on_key(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_target_obj(e), *to = NULL;
	uint32_t key = lv_event_get_key(e);
	int t = index_of(home->tiles, home->n_tiles, obj);
	int b = index_of(home->buttons, home->n_buttons, obj);

	if (key == LV_KEY_LEFT)
		to = t > 0 ? home->tiles[t - 1] : b > 0 ? home->buttons[b - 1] : NULL;
	else if (key == LV_KEY_RIGHT)
		to = t >= 0 && t + 1 < home->n_tiles ? home->tiles[t + 1]
		     : b >= 0 && b + 1 < home->n_buttons ? home->buttons[b + 1] : NULL;
	else if (key == LV_KEY_DOWN && t >= 0 && home->n_buttons)
		to = home->buttons[home->last_button];
	else if (key == LV_KEY_UP && b >= 0 && home->n_tiles)
		to = home->tiles[home->last_tile];
	if (to)
		lv_group_focus_obj(to);
}

static void focus_frame(lv_obj_t *obj, int32_t radius)
{
	lv_obj_set_style_radius(obj, radius, 0);
	lv_obj_set_style_outline_width(obj, FOCUS_W, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_outline_color(obj, COLOR_ACCENT, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_outline_pad(obj, FOCUS_PAD, LV_STATE_FOCUS_KEY);
	lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
}

static lv_obj_t *tile_new(lv_obj_t *parent, const struct entry *en, bool minimized)
{
	lv_obj_t *t = lv_button_create(parent), *o;
	const char *title = entry_text(en), *cover = game_cover(en->game);
	char src[PATH_MAX + 4];
	lv_color_t c = title_color(title);

	lv_obj_remove_style_all(t);
	lv_obj_set_size(t, home->tile, home->tile);
	focus_frame(t, TILE_RADIUS);
	lv_obj_set_style_clip_corner(t, true, 0);
	lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(t, c, 0);
	lv_obj_set_style_bg_grad_color(t, lv_color_darken(c, LV_OPA_50), 0);
	lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_text_color(t, lv_color_white(), 0);
	lv_obj_set_scrollable(t, false);

	if (cover && snprintf(src, sizeof(src), "A:%s", cover) < (int)sizeof(src)) {
		o = lv_image_create(t);
		lv_image_set_src(o, src);
		lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
		lv_image_set_inner_align(o, LV_IMAGE_ALIGN_COVER);
	} else {
		o = lv_label_create(t);
		lv_obj_set_width(o, home->tile - 20);
		lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_font(o, home->tile < TILE_SMALL ? UI_FONT_SMALL : UI_FONT_MID, 0);
		lv_label_set_text(o, title);
		lv_obj_center(o);
		o = lv_label_create(t);
		lv_obj_set_style_text_opa(o, LV_OPA_50, 0);
		lv_label_set_text(o, LV_SYMBOL_PLAY);
		lv_obj_align(o, LV_ALIGN_TOP_LEFT, 10, 8);
	}
	if (game_missing_text(en->game, src, sizeof(src))) {
		/* dimmed under a veil: the focus frame stays bright */
		o = lv_obj_create(t);
		lv_obj_remove_style_all(o);
		lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
		lv_obj_set_style_bg_color(o, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(o, LV_OPA_60, 0);
		o = lv_label_create(t);
		lv_obj_set_style_text_font(o, UI_FONT_SMALL, 0);
		lv_obj_set_style_bg_color(o, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(o, LV_OPA_70, 0);
		lv_obj_set_style_pad_hor(o, 6, 0);
		lv_obj_set_style_radius(o, 4, 0);
		/* a small tile has no room for the words */
		lv_label_set_text(o, home->tile < TILE_SMALL ? LV_SYMBOL_WARNING : tr(S_HOME_NOT_INSTALLED));
		lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, -8);
	}
	if (minimized) {
		o = lv_label_create(t);
		lv_obj_set_size(o, 34, 34);
		lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(o, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(o, LV_OPA_70, 0);
		lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_top(o, 8, 0);
		lv_label_set_text(o, LV_SYMBOL_PAUSE);
		lv_obj_align(o, LV_ALIGN_TOP_RIGHT, -8, 8);
	}
	lv_obj_add_event_cb(t, on_click, LV_EVENT_CLICKED, (void *)en);
	lv_obj_add_event_cb(t, on_focused, LV_EVENT_FOCUSED, (void *)en);
	lv_obj_add_event_cb(t, on_key, LV_EVENT_KEY, NULL);
	return t;
}

static lv_obj_t *round_new(lv_obj_t *parent, const struct entry *en, int32_t size)
{
	lv_obj_t *b = lv_button_create(parent), *label;

	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, size, size);
	focus_frame(b, LV_RADIUS_CIRCLE);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, COLOR_ROUND, 0);
	lv_obj_set_style_bg_color(b, lv_color_lighten(COLOR_ROUND, LV_OPA_20), LV_STATE_FOCUS_KEY);
	label = lv_label_create(b);
	lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
	lv_obj_set_style_text_color(label, en->action == ACT_POWER_OFF ? UI_COLOR_WARN
					    : lv_color_white(), 0);
	lv_label_set_text(label, en->symbol);
	lv_obj_center(label);
	lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, (void *)en);
	lv_obj_add_event_cb(b, on_focused, LV_EVENT_FOCUSED, (void *)en);
	lv_obj_add_event_cb(b, on_key, LV_EVENT_KEY, NULL);
	return b;
}

/* "(A) text" in the bottom right corner */
static lv_obj_t *footer_new(lv_obj_t *scr, int32_t w, int32_t h, int32_t footer_h)
{
	lv_obj_t *line = lv_obj_create(scr), *box, *badge;

	lv_obj_remove_style_all(line);
	lv_obj_set_size(line, w - 2 * 24, 1);
	lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(line, COLOR_LINE, 0);
	lv_obj_set_pos(line, 24, h - footer_h);

	box = lv_obj_create(scr);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, LV_SIZE_CONTENT, footer_h);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(box, 8, 0);
	lv_obj_align(box, LV_ALIGN_BOTTOM_RIGHT, -28, 0);

	badge = lv_label_create(box);
	lv_obj_set_size(badge, 22, 22);
	lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
	lv_obj_set_style_text_color(badge, COLOR_BG, 0);
	lv_obj_set_style_text_font(badge, UI_FONT_SMALL, 0);
	lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_top(badge, 2, 0);
	lv_label_set_text(badge, "A");

	home->action = lv_label_create(box);
	lv_obj_set_style_text_font(home->action, UI_FONT_SMALL, 0);
	lv_label_set_text(home->action, "");
	return box;
}

static void on_screen_delete(lv_event_t *e)
{
	struct home *h = lv_event_get_user_data(e);

	if (port_input_2d() == h->group)
		port_input_set_2d(NULL);
	if (home == h)
		home = NULL;
	free(h);
}

static void *home_init(lv_obj_t *parent, lv_obj_t **root)
{
	lv_display_t *disp = lv_display_get_default();
	int32_t w = lv_display_get_horizontal_resolution(disp);
	int32_t h = lv_display_get_vertical_resolution(disp);
	int32_t footer_h = LV_MIN(FOOTER_H_MAX, h / 9), round = h >= 400 ? 52 : 40;
	int32_t title_h = lv_font_get_line_height(UI_FONT_MID);
	int32_t name_h = lv_font_get_line_height(UI_FONT_SMALL);
	int32_t frame = FOCUS_W + FOCUS_PAD, margin = w >= 640 ? 44 : 20;
	int32_t avail, fixed, top, gap;
	lv_obj_t *scr, *bar, *focus = NULL;
	size_t i;

	(void)parent;
	build_menu();
	home = calloc(1, sizeof(*home));
	if (!home)
		return NULL;
	home->group = lv_group_get_default();

	scr = lv_obj_create(NULL);
	home->scr = scr;
	lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
	lv_obj_set_style_bg_grad_color(scr, COLOR_BG2, 0);
	lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_text_color(scr, lv_color_white(), 0);
	lv_obj_set_scrollable(scr, false);
	lv_obj_add_event_cb(scr, on_screen_delete, LV_EVENT_DELETE, home);
	bar = statusbar_create(scr, NULL, "");	/* the time, volume, Wi-Fi, battery */
	if (bar)
		lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);

	/* title, the tiles, the round buttons with the name under them: the
	 * tiles as big as the rest leaves room for, the whole block centred */
	avail = h - UI_STATUSBAR_H - footer_h;
	fixed = title_h + 2 * frame + round + 2 * frame + name_h + 3 * 10;
	home->tile = LV_CLAMP(TILE_MIN, avail - fixed, TILE_MAX);
	gap = LV_MAX(10, (avail - fixed - home->tile) / 5 + 10);
	top = UI_STATUSBAR_H + LV_MAX(0, (avail - (fixed - 3 * 10 + 3 * gap) - home->tile) / 2);

	home->title = lv_label_create(scr);
	lv_obj_set_style_text_font(home->title, UI_FONT_MID, 0);
	lv_obj_set_style_text_color(home->title, COLOR_ACCENT, 0);
	lv_obj_set_width(home->title, w - 2 * margin);
	lv_label_set_long_mode(home->title, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(home->title, n_games ? "" : tr(S_HOME_NO_GAMES));
	lv_obj_set_pos(home->title, margin, top);

	home->row = lv_obj_create(scr);
	lv_obj_remove_style_all(home->row);
	lv_obj_set_size(home->row, w, home->tile + 2 * frame);
	lv_obj_set_pos(home->row, 0, top + title_h + gap - frame);
	lv_obj_set_flex_flow(home->row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(home->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_left(home->row, margin, 0);
	lv_obj_set_style_pad_right(home->row, margin, 0);
	lv_obj_set_style_pad_column(home->row, TILE_GAP, 0);
	lv_obj_set_scroll_dir(home->row, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(home->row, LV_SCROLLBAR_MODE_OFF);

	home->rounds = lv_obj_create(scr);
	lv_obj_remove_style_all(home->rounds);
	lv_obj_set_size(home->rounds, w, round + 2 * frame);
	lv_obj_set_pos(home->rounds, 0, top + title_h + gap + home->tile + frame + gap);
	lv_obj_set_flex_flow(home->rounds, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(home->rounds, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(home->rounds, ROUND_GAP, 0);
	lv_obj_set_scrollable(home->rounds, false);

	home->round_name = lv_label_create(scr);
	lv_obj_set_style_text_font(home->round_name, UI_FONT_SMALL, 0);
	lv_obj_set_style_text_color(home->round_name, UI_COLOR_DIM, 0);
	lv_label_set_text(home->round_name, "");
	lv_obj_set_hidden(home->round_name, true);
	lv_obj_align_to(home->round_name, home->rounds, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
	lv_obj_set_width(home->round_name, w);
	lv_obj_set_style_text_align(home->round_name, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_x(home->round_name, 0);

	footer_new(scr, w, h, footer_h);

	for (i = 0; i < n_menu; i++) {
		bool minimized = view_video_minimized() != VIDEO_NONE && running_entry() == &menu[i];
		lv_obj_t *o;

		if (menu[i].game.def) {
			o = tile_new(home->row, &menu[i], minimized);
			home->tile_entry[home->n_tiles] = &menu[i];
			home->tiles[home->n_tiles++] = o;
			if (minimized)
				focus = o;
		} else {
			o = round_new(home->rounds, &menu[i], round);
			home->button_entry[home->n_buttons] = &menu[i];
			home->buttons[home->n_buttons++] = o;
		}
	}
	port_input_set_2d(home->group);
	/* the game that is minimised, else the first one */
	if (!focus)
		focus = home->n_tiles ? home->tiles[0] : home->n_buttons ? home->buttons[0] : NULL;
	if (focus) {
		int t = index_of(home->tiles, home->n_tiles, focus);

		/* the group focused its first object before it had our events */
		lv_group_focus_obj(focus);
		lv_obj_update_layout(scr);
		focus_changed(focus, t >= 0 ? home->tile_entry[t]
				    : home->button_entry[index_of(home->buttons, home->n_buttons, focus)]);
		lv_obj_scroll_to_view(focus, LV_ANIM_OFF);
	}
	*root = scr;
	return NULL;	/* the state goes with the screen (on_screen_delete) */
}

const struct widget_ops home_screen = {
	.name = "home",
	.init = home_init,
};
