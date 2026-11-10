/*
 * Settings, laid out like a console's system settings: the categories in a
 * column on the left, the settings of the chosen one on the right, what the
 * buttons do at the bottom. Up and Down go along a column (the screen's
 * group gets them as keys, port_input_set_2d()); Right or A goes into the
 * settings, Left or B back to the categories, B there closes the screen.
 * In the settings Left and Right move a slider, A changes a choice or
 * opens the screen of a link.
 *
 *   Screen    brightness (CTX_DISPLAY)
 *   Sound     volume, output (CTX_AUDIO)
 *   Internet  the Wi-Fi network: opens the Wi-Fi screen; the address (CTX_WIFI);
 *             the stream on or off, its address and viewers (CONFIG_STREAM)
 *   Games     the controls hint when a game starts (CTX_SETTINGS)
 *   System    language (CTX_SETTINGS), device, platform (CTX_SYSTEM),
 *             battery (CTX_POWER)
 *
 * Only the chosen category's rows exist; it is kept while the application
 * runs, so the screen rebuilt in another language shows the same one.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../widget.h"
#include "gui/action.h"
#ifdef CONFIG_STREAM
#include "stream/stream.h"
#endif

#define STEP 5		/* percent per slider step */
#define SIDE_CELLS 7	/* the categories' column */
#define ROW_CELLS 2	/* a category, a setting at least */

enum row_id {
	R_BRIGHTNESS, R_VOLUME, R_OUTPUT, R_WIFI, R_IP, R_STREAM, R_GAME_HINT, R_LANG, R_DEVICE, R_PLATFORM,
	R_BATTERY, R_COUNT
};

enum row_kind {
	ROW_SLIDER,	/* Left, Right */
	ROW_CHOICE,	/* A: the next value */
	ROW_LINK,	/* A: another screen */
	ROW_INFO,	/* not focusable */
};

struct settings;

struct row {
	struct settings *s;
	enum row_id id;
	enum row_kind kind;
	lv_obj_t *obj, *value, *slider;		/* obj NULL: not in the panel */
};

struct category {
	const char *symbol;
	enum str_id name;
	void (*build)(struct settings *s);
};

struct settings {
	lv_obj_t *panel, *footer;
	lv_obj_t *cats[6];
	lv_group_t *group;
	int cat;			/* in the panel */
	int32_t row_h;
	struct row rows[R_COUNT];
	int shown_brightness, shown_volume;	/* -2: nothing yet */
	enum audio_output output;
	bool game_hint;
	bool stream;
	lv_timer_t *viewers;	/* the stream's row: its viewers change */
};

static int kept_cat;	/* the category last shown */
static int kept_row = -1;	/* the link row whose screen was opened: focused again */

/* ---- rows --------------------------------------------------------------- */

static bool row_shown(const struct row *r)
{
	return r->obj && !lv_obj_is_hidden(r->obj);
}

static void focus_cat(struct settings *s)
{
	lv_group_focus_obj(s->cats[s->cat]);
}

/* The next focusable row from obj (NULL: from the top) in dir (+1, -1) */
static lv_obj_t *row_next(struct settings *s, lv_obj_t *obj, int dir)
{
	int32_t n = (int32_t)lv_obj_get_child_count(s->panel);
	int32_t i = obj ? lv_obj_get_index(obj) + dir : dir > 0 ? 0 : n - 1;

	for (; i >= 0 && i < n; i += dir) {
		lv_obj_t *o = lv_obj_get_child(s->panel, i);

		if (lv_obj_get_group(o) && !lv_obj_is_hidden(o))
			return o;
	}
	return NULL;
}

static void show_slider(struct row *r, int percent)
{
	char text[8];

	lv_slider_set_value(r->slider, (percent + STEP / 2) / STEP, LV_ANIM_OFF);
	snprintf(text, sizeof(text), "%d%%", percent);
	ui_set_text(r->value, text);
}

/* A or Left, Right (dir 0: A) on a row */
static void row_change(struct row *r, int dir)
{
	struct settings *s = r->s;
	int v;

	if (r->kind == ROW_SLIDER) {
		if (!dir || lv_obj_is_hidden(r->slider))
			return;
		v = (int)lv_slider_get_value(r->slider) + dir;
		if (v < lv_slider_get_min_value(r->slider) || v > lv_slider_get_max_value(r->slider))
			return;
		/* at once, the context follows (the same value: shown already) */
		show_slider(r, v * STEP);
		if (r->id == R_BRIGHTNESS) {
			s->shown_brightness = v * STEP;
			gui_action_post(ACT_SET_BRIGHTNESS, v * STEP);
		} else {
			s->shown_volume = v * STEP;
			gui_action_post(ACT_SET_VOLUME, v * STEP);
		}
		return;
	}
	if (dir)
		return;
	switch (r->id) {
	case R_OUTPUT:
		gui_action_post(ACT_SET_AUDIO_OUT, (s->output + 1) % AUDIO_OUT_COUNT);
		break;
	case R_LANG:
		gui_action_post_str(ACT_SET_LANG, i18n_next(i18n_lang()));
		break;
	case R_GAME_HINT:
		gui_action_post(ACT_SET_GAME_HINT, !s->game_hint);
		break;
	case R_STREAM:
		gui_action_post(ACT_SET_STREAM, !s->stream);
		break;
	case R_WIFI:
		kept_row = R_WIFI;
		gui_action_post(ACT_OPEN_SCREEN, SCREEN_WIFI);
		break;
	default:
		break;
	}
}

static void row_hints(struct row *r)
{
	const char *back = tr(S_BACK);

	if (r->kind == ROW_SLIDER && !lv_obj_is_hidden(r->slider))
		footer_set(r->s->footer, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT, tr(S_CHANGE), "B", back, NULL);
	else if (r->kind == ROW_CHOICE)
		footer_set(r->s->footer, "B", back, "A", tr(S_CHANGE), NULL);
	else if (r->kind == ROW_LINK)
		footer_set(r->s->footer, "B", back, "A", tr(S_OK), NULL);
	else
		footer_set(r->s->footer, "B", back, NULL);
}

static void on_row_focused(lv_event_t *e)
{
	struct row *r = lv_event_get_user_data(e);

	lv_obj_scroll_to_view(r->obj, LV_ANIM_ON);
	row_hints(r);
}

static void on_row_key(lv_event_t *e)
{
	struct row *r = lv_event_get_user_data(e);
	lv_obj_t *to;

	switch (lv_event_get_key(e)) {
	case LV_KEY_UP:
	case LV_KEY_DOWN:
		to = row_next(r->s, r->obj, lv_event_get_key(e) == LV_KEY_UP ? -1 : 1);
		if (to)
			lv_group_focus_obj(to);
		break;
	case LV_KEY_LEFT:
		if (r->kind == ROW_SLIDER && !lv_obj_is_hidden(r->slider))
			row_change(r, -1);
		else
			focus_cat(r->s);
		break;
	case LV_KEY_RIGHT:
		row_change(r, 1);
		break;
	default:
		break;
	}
}

static void on_row_click(lv_event_t *e)
{
	row_change(lv_event_get_user_data(e), 0);
}

/* B in the settings: back to the categories, not out of the screen */
static void on_row_cancel(lv_event_t *e)
{
	struct row *r = lv_event_get_user_data(e);

	port_input_take_back();
	focus_cat(r->s);
}

static lv_obj_t *label_new(lv_obj_t *parent, lv_color_t color)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, UI_FONT_MID, 0);
	lv_obj_set_style_text_color(l, color, 0);
	lv_label_set_text(l, "");
	return l;
}

/* name on the left, the value on the right, a slider under them */
static struct row *row_new(struct settings *s, enum row_id id, enum row_kind kind,
			   enum str_id name)
{
	struct row *r = &s->rows[id];
	lv_obj_t *o = lv_obj_create(s->panel), *label;

	*r = (struct row){ .s = s, .id = id, .kind = kind, .obj = o };
	lv_obj_remove_style_all(o);
	lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(o, s->row_h, 0);
	lv_obj_set_style_pad_hor(o, ui_cell() / 2, 0);
	lv_obj_set_style_pad_ver(o, ui_px(8), 0);
	lv_obj_set_style_pad_row(o, ui_px(10), 0);
	lv_obj_set_style_pad_column(o, ui_px(8), 0);
	lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_width(o, 1, 0);
	lv_obj_set_style_border_color(o, UI_COLOR_LINE, 0);
	/* the name never wraps: a value too long for its line goes under it */
	lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(o, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(o, false);

	label = label_new(o, lv_color_white());
	lv_label_set_text(label, tr(name));
	r->value = label_new(o, kind == ROW_CHOICE ? UI_COLOR_ACCENT : UI_COLOR_DIM);
	lv_obj_set_style_max_width(r->value, LV_PCT(100), 0);
	lv_label_set_long_mode(r->value, LV_LABEL_LONG_MODE_WRAP);

	if (kind == ROW_SLIDER) {
		lv_obj_t *sl = lv_slider_create(o);

		lv_group_remove_obj(sl);	/* the row moves it */
		lv_obj_set_clickable(sl, false);
		/* as wide as the text, drawn a knob's half shorter at each
		 * end: the knob reaches past the track's ends by that, and
		 * stays clear of the focus frame */
		lv_obj_set_size(sl, LV_PCT(100), ui_px(6));
		lv_obj_set_style_transform_width(sl, -(ui_px(6) / 2 + ui_px(6)), 0);
		lv_obj_set_style_margin_bottom(sl, ui_px(6), 0);
		lv_obj_set_style_bg_color(sl, lv_color_lighten(UI_COLOR_LINE, LV_OPA_20), LV_PART_MAIN);
		lv_obj_set_style_bg_color(sl, UI_COLOR_ACCENT, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
		lv_obj_set_style_pad_all(sl, ui_px(6), LV_PART_KNOB);
		lv_obj_set_style_shadow_width(sl, 0, LV_PART_KNOB);
		lv_slider_set_range(sl, 0, 100 / STEP);
		r->slider = sl;
	}
	if (kind != ROW_INFO) {
		console_focus_box(o);
		lv_group_add_obj(s->group, o);
		lv_obj_add_event_cb(o, on_row_focused, LV_EVENT_FOCUSED, r);
		lv_obj_add_event_cb(o, on_row_key, LV_EVENT_KEY, r);
		lv_obj_add_event_cb(o, on_row_click, LV_EVENT_CLICKED, r);
		lv_obj_add_event_cb(o, on_row_cancel, LV_EVENT_CANCEL, r);
	}
	return r;
}

/* ---- the categories ----------------------------------------------------- */

static void build_screen(struct settings *s)
{
	row_new(s, R_BRIGHTNESS, ROW_SLIDER, S_BRIGHTNESS);
	lv_slider_set_range(s->rows[R_BRIGHTNESS].slider, 1, 100 / STEP);
}

#ifdef CONFIG_AUDIO
static void build_sound(struct settings *s)
{
	row_new(s, R_VOLUME, ROW_SLIDER, S_VOLUME);
	row_new(s, R_OUTPUT, ROW_CHOICE, S_OUTPUT);
}
#endif

#if defined(CONFIG_WIFI) && defined(CONFIG_SCREEN_WIFI)
static void build_internet(struct settings *s)
{
	struct row *r = row_new(s, R_WIFI, ROW_LINK, S_WIFI);

	lv_obj_set_style_text_color(r->value, lv_color_white(), 0);
	row_new(s, R_IP, ROW_INFO, S_IP_ADDRESS);
#ifdef CONFIG_STREAM
	row_new(s, R_STREAM, ROW_CHOICE, S_STREAM);
#endif
}
#endif

#ifdef CONFIG_GAMES
static void build_games(struct settings *s)
{
	row_new(s, R_GAME_HINT, ROW_CHOICE, S_GAME_HINT);
}
#endif

static void build_system(struct settings *s)
{
	if (i18n_count() > 1)
		row_new(s, R_LANG, ROW_CHOICE, S_LANGUAGE);
	row_new(s, R_DEVICE, ROW_INFO, S_DEVICE);
	row_new(s, R_PLATFORM, ROW_INFO, S_PLATFORM);
	row_new(s, R_BATTERY, ROW_INFO, S_BATTERY);
}

static const struct category categories[] = {
	{ LV_SYMBOL_EYE_OPEN, S_SET_SCREEN, build_screen },
#ifdef CONFIG_AUDIO
	{ LV_SYMBOL_VOLUME_MAX, S_SET_SOUND, build_sound },
#endif
#if defined(CONFIG_WIFI) && defined(CONFIG_SCREEN_WIFI)
	{ LV_SYMBOL_WIFI, S_SET_INTERNET, build_internet },
#endif
#ifdef CONFIG_GAMES
	{ LV_SYMBOL_PLAY, S_SET_GAMES, build_games },
#endif
	{ LV_SYMBOL_LIST, S_SET_SYSTEM, build_system },
};

#define N_CATS ((int)(sizeof(categories) / sizeof(categories[0])))
_Static_assert(N_CATS <= (int)(sizeof(((struct settings *)0)->cats) / sizeof(lv_obj_t *)),
	       "struct settings: more cats[]");

static void settings_update(void *self, const struct gui_ctx *c, uint32_t changed);
#ifdef CONFIG_STREAM
static void on_viewers(lv_timer_t *t);
#endif

static void show_cat(struct settings *s, int i)
{
	lv_obj_remove_state(s->cats[s->cat], LV_STATE_CHECKED);
	s->cat = kept_cat = i;
	lv_obj_add_state(s->cats[i], LV_STATE_CHECKED);
	lv_obj_clean(s->panel);		/* its rows leave the group too */
	memset(s->rows, 0, sizeof(s->rows));
	s->shown_brightness = -2;
	s->shown_volume = -2;
	categories[i].build(s);
	lv_obj_scroll_to_y(s->panel, 0, LV_ANIM_OFF);
	settings_update(s, view_ctx(), CTX_ALL);
}

static void cat_focused(struct settings *s, int i)
{
	if (i != s->cat)
		show_cat(s, i);
	if (row_next(s, NULL, 1))
		footer_set(s->footer, "B", tr(S_BACK), "A", tr(S_OK), NULL);
	else
		footer_set(s->footer, "B", tr(S_BACK), NULL);
}

static int cat_index(lv_event_t *e)
{
	return (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
}

static void on_cat_focused(lv_event_t *e)
{
	cat_focused(lv_event_get_user_data(e), cat_index(e));
}

static void enter_panel(struct settings *s)
{
	lv_obj_t *first = row_next(s, NULL, 1);

	if (first)
		lv_group_focus_obj(first);
}

static void on_cat_key(lv_event_t *e)
{
	struct settings *s = lv_event_get_user_data(e);
	int i = cat_index(e);

	switch (lv_event_get_key(e)) {
	case LV_KEY_UP:
		if (i > 0)
			lv_group_focus_obj(s->cats[i - 1]);
		break;
	case LV_KEY_DOWN:
		if (i + 1 < N_CATS)
			lv_group_focus_obj(s->cats[i + 1]);
		break;
	case LV_KEY_RIGHT:
		enter_panel(s);
		break;
	default:
		break;
	}
}

static void on_cat_click(lv_event_t *e)
{
	enter_panel(lv_event_get_user_data(e));
}

static lv_obj_t *cat_new(struct settings *s, lv_obj_t *parent, int i)
{
	lv_obj_t *o = lv_obj_create(parent), *label;

	lv_obj_remove_style_all(o);
	lv_obj_set_size(o, LV_PCT(100), s->row_h);	/* level with the rows */
	lv_obj_set_user_data(o, (void *)(intptr_t)i);
	lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_left(o, ui_cell() / 2, 0);
	lv_obj_set_style_pad_column(o, ui_px(8), 0);
	lv_obj_set_scrollable(o, false);
	lv_obj_set_style_text_font(o, UI_FONT_MID, 0);
	lv_obj_set_style_text_color(o, lv_color_white(), 0);
	/* the one shown: a bar at its left and the accent colour */
	lv_obj_set_style_text_color(o, UI_COLOR_ACCENT, LV_STATE_CHECKED);
	lv_obj_set_style_border_side(o, LV_BORDER_SIDE_LEFT, LV_STATE_CHECKED);
	lv_obj_set_style_border_width(o, ui_px(4), LV_STATE_CHECKED);
	lv_obj_set_style_border_color(o, UI_COLOR_ACCENT, LV_STATE_CHECKED);
	console_focus_box(o);

	label = lv_label_create(o);
	lv_obj_set_width(label, ui_cell());
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(label, categories[i].symbol);
	label = lv_label_create(o);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_set_height(label, lv_font_get_line_height(UI_FONT_MID));	/* one line, else "..." */
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(label, tr(categories[i].name));

	lv_group_add_obj(s->group, o);	/* the first one is focused here, without events */
	lv_obj_add_event_cb(o, on_cat_focused, LV_EVENT_FOCUSED, s);
	lv_obj_add_event_cb(o, on_cat_key, LV_EVENT_KEY, s);
	lv_obj_add_event_cb(o, on_cat_click, LV_EVENT_CLICKED, s);
	return o;
}

/* ---- the screen ---------------------------------------------------------- */

static lv_obj_t *column(lv_obj_t *scr, int32_t x, int32_t y, int32_t w, int32_t h)
{
	lv_obj_t *o = lv_obj_create(scr);

	lv_obj_remove_style_all(o);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
	return o;
}

static lv_obj_t *line(lv_obj_t *scr, int32_t x, int32_t y, int32_t w, int32_t h)
{
	lv_obj_t *o = lv_obj_create(scr);

	lv_obj_remove_style_all(o);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(o, UI_COLOR_LINE, 0);
	return o;
}

/*
 * On the grid (widgets/console.c): the body between the header's and the
 * footer's lines, the categories SIDE_CELLS wide at the left margin, the
 * line between the columns in the middle of the next cell, the settings
 * from the cell after it to the right margin. A category and a setting
 * are ROW_CELLS high, so they are level.
 */
static void *settings_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct settings *s = calloc(1, sizeof(*s));
	int32_t u = ui_cell(), side_w = SIDE_CELLS * u;
	lv_obj_t *scr, *side;
	lv_area_t body;
	int i;

	(void)parent;
	if (!s)
		return NULL;
	s->group = lv_group_get_default();
	s->row_h = ROW_CELLS * u;
	s->output = -1;
	scr = console_screen(LV_SYMBOL_SETTINGS, tr(S_SETTINGS));
	s->footer = footer_create(scr);

	ui_body_area(&body);
	side = column(scr, body.x1, body.y1, side_w, lv_area_get_height(&body));
	lv_obj_set_scrollable(side, false);
	line(scr, body.x1 + side_w + u / 2, body.y1, 1, lv_area_get_height(&body));
	s->panel = column(scr, body.x1 + side_w + u, body.y1, lv_area_get_width(&body) - side_w - u,
			  lv_area_get_height(&body));
	lv_obj_set_scroll_dir(s->panel, LV_DIR_VER);

	for (i = 0; i < N_CATS; i++)
		s->cats[i] = cat_new(s, side, i);
	s->cat = kept_cat < N_CATS ? kept_cat : 0;
	show_cat(s, s->cat);
	lv_group_focus_obj(s->cats[s->cat]);
	cat_focused(s, s->cat);	/* no event if it was focused already */
	if (kept_row >= 0 && row_shown(&s->rows[kept_row]))
		lv_group_focus_obj(s->rows[kept_row].obj);
	kept_row = -1;
	port_input_set_2d(s->group);
#ifdef CONFIG_STREAM
	s->viewers = lv_timer_create(on_viewers, 1000, s);
#endif
	*root = scr;
	return s;
}

/* ---- what the context says ----------------------------------------------- */

static void update_slider(struct row *r, bool has, int percent, int *shown)
{
	lv_obj_set_hidden(r->slider, !has);
	if (!has) {
		ui_set_text(r->value, tr(S_NOT_AVAILABLE));
		*shown = -2;
	} else if (percent != *shown) {
		*shown = percent;
		show_slider(r, percent);	/* no VALUE_CHANGED, so no loop back */
	}
}

static void update_audio(struct settings *s, const struct gui_ctx *c)
{
	static const enum str_id out_names[AUDIO_OUT_COUNT] = {
		S_OUT_AUTO, S_OUT_SPEAKER, S_OUT_HEADPHONES,
	};
	bool has = c->audio.available && c->audio.volume >= 0;
	char text[64];

	update_slider(&s->rows[R_VOLUME], has, c->audio.volume, &s->shown_volume);
	lv_obj_set_hidden(s->rows[R_OUTPUT].obj, !has || !c->audio.can_route);
	s->output = c->audio.output;
	/* "Auto" says where the sound goes now */
	if (c->audio.output == AUDIO_OUT_AUTO)
		snprintf(text, sizeof(text), tr(S_OUT_AUTO_NOW_FMT),
			 tr(c->audio.headphones ? S_NOW_HEADPHONES : S_NOW_SPEAKER));
	else
		snprintf(text, sizeof(text), "%s", tr(out_names[c->audio.output]));
	ui_set_text(s->rows[R_OUTPUT].value, text);
}

static void update_wifi(struct settings *s, const struct gui_ctx *c)
{
	const char *text;
	char buf[WIFI_SSID_MAX + 8];
	bool ip = false;

	if (!c->wifi.available) {
		text = tr(S_WIFI_UNAVAILABLE);
	} else if (!c->wifi.enabled || c->wifi.link == WIFI_OFF) {
		text = tr(S_WIFI_OFF);
	} else if (c->wifi.link == WIFI_CONNECTED) {
		text = c->wifi.ssid;
		ip = c->wifi.ip[0];
	} else if (c->wifi.link == WIFI_CONNECTING) {
		text = "...";
	} else {
		text = tr(S_WIFI_DISCONNECTED);
	}
	snprintf(buf, sizeof(buf), "%s  " LV_SYMBOL_RIGHT, text);
	ui_set_text(s->rows[R_WIFI].value, buf);
	lv_obj_set_hidden(s->rows[R_IP].obj, !ip);
	if (ip)
		ui_set_text(s->rows[R_IP].value, c->wifi.ip);
}

static void update_system(struct settings *s, const struct gui_ctx *c)
{
	char text[64];
	bool battery = c->power.available && c->power.present && c->power.capacity >= 0;

	if (s->rows[R_LANG].obj)
		ui_set_text(s->rows[R_LANG].value, tr(S_LANG_NAME));
	lv_obj_set_hidden(s->rows[R_DEVICE].obj, !c->system.model[0]);
	ui_set_text(s->rows[R_DEVICE].value, c->system.model);
	ui_set_text(s->rows[R_PLATFORM].value, c->system.platform);
	lv_obj_set_hidden(s->rows[R_BATTERY].obj, !battery);
	if (battery) {
		if (c->power.charging)
			snprintf(text, sizeof(text), "%d%%, %s", c->power.capacity, tr(S_CHARGING));
		else
			snprintf(text, sizeof(text), "%d%%", c->power.capacity);
		ui_set_text(s->rows[R_BATTERY].value, text);
	}
}

#ifdef CONFIG_STREAM
/* "Off", or where to open it and who watches */
static void update_stream(struct settings *s, const struct gui_ctx *c)
{
	char text[96];
	int n = stream_viewers();

	s->stream = c->settings.stream;
	if (!s->stream)
		snprintf(text, sizeof(text), "%s", tr(S_OFF));
	else if (!stream_running())
		snprintf(text, sizeof(text), "%s", tr(S_STREAM_FAILED));
	else
		snprintf(text, sizeof(text), "rtsp://%s:8554/live%s%s",
			 c->wifi.ip[0] ? c->wifi.ip : "<IP>", n ? "  " LV_SYMBOL_EYE_OPEN " " : "",
			 n ? (n == 1 ? "1" : n == 2 ? "2" : n == 3 ? "3" : "4") : "");
	ui_set_text(s->rows[R_STREAM].value, text);
}

static void on_viewers(lv_timer_t *t)
{
	struct settings *s = lv_timer_get_user_data(t);

	if (s->rows[R_STREAM].obj)
		update_stream(s, view_ctx());
}
#endif

static void update_games(struct settings *s, const struct gui_ctx *c)
{
	s->game_hint = c->settings.game_hint;
	ui_set_text(s->rows[R_GAME_HINT].value, tr(s->game_hint ? S_ON : S_OFF));
}

static void settings_update(void *self, const struct gui_ctx *c, uint32_t changed)
{
	struct settings *s = self;
	lv_obj_t *f;

	if (!c)
		return;
	if ((changed & CTX_DISPLAY) && s->rows[R_BRIGHTNESS].obj)
		update_slider(&s->rows[R_BRIGHTNESS], c->display.has_backlight && c->display.brightness >= 0,
			      c->display.brightness, &s->shown_brightness);
	if ((changed & CTX_AUDIO) && s->rows[R_VOLUME].obj)
		update_audio(s, c);
	if ((changed & CTX_WIFI) && s->rows[R_WIFI].obj)
		update_wifi(s, c);
	if ((changed & (CTX_SYSTEM | CTX_POWER | CTX_SETTINGS)) && s->rows[R_PLATFORM].obj)
		update_system(s, c);
	if ((changed & CTX_SETTINGS) && s->rows[R_GAME_HINT].obj)
		update_games(s, c);
#ifdef CONFIG_STREAM
	if ((changed & (CTX_SETTINGS | CTX_WIFI)) && s->rows[R_STREAM].obj)
		update_stream(s, c);
#endif
	/* a row that went away: back to the categories */
	f = lv_group_get_focused(s->group);
	if (f && lv_obj_is_hidden(f))
		focus_cat(s);
	else if (f && lv_obj_get_parent(f) == s->panel && (changed & (CTX_DISPLAY | CTX_AUDIO)))
		for (int i = 0; i < R_COUNT; i++)
			if (s->rows[i].obj == f)
				row_hints(&s->rows[i]);	/* a slider may have come or gone */
}

static void settings_deinit(void *self)
{
	struct settings *s = self;

	if (s->viewers)
		lv_timer_delete(s->viewers);
	if (port_input_2d() == s->group)
		port_input_set_2d(NULL);
	/* LVGL deletes the children after this: none of them may take the
	 * focus (and call us) as the one focused goes */
	lv_group_remove_all_objs(s->group);
	free(s);
}

const struct widget_ops settings_screen = {
	.name = "settings",
	.deps = CTX_DISPLAY | CTX_AUDIO | CTX_WIFI | CTX_SYSTEM | CTX_POWER | CTX_SETTINGS,
	.init = settings_init,
	.update = settings_update,
	.deinit = settings_deinit,
};
