/*
 * Settings: brightness (CTX_DISPLAY), volume and output (CTX_AUDIO),
 * language, Wi-Fi status that opens the Wi-Fi screen (CTX_WIFI). Sized to
 * fit 320 lines without scrolling; the battery is in the status bar.
 */
#include <stdlib.h>

#include "../widget.h"
#include "gui/action.h"

#define STEP 5		/* percent per slider step */
#define ROW_PAD 4	/* above and below the content of a row */

struct settings {
	lv_obj_t *slider, *value, *no_bl;
	int shown_brightness;
	lv_obj_t *vol_row, *vol_slider, *vol_value, *out_row, *out_label;
	int shown_volume;
	enum audio_output output;
	lv_obj_t *wifi_row, *wifi_label;
};

static void on_lang(lv_event_t *e)
{
	(void)e;
	gui_action_post_str(ACT_SET_LANG, i18n_next(i18n_lang()));
}

static void on_slider(lv_event_t *e)
{
	lv_obj_t *slider = lv_event_get_target(e);

	gui_action_post(ACT_SET_BRIGHTNESS, (int)lv_slider_get_value(slider) * STEP);
}

#ifdef CONFIG_AUDIO
static void on_volume(lv_event_t *e)
{
	lv_obj_t *slider = lv_event_get_target(e);

	gui_action_post(ACT_SET_VOLUME, (int)lv_slider_get_value(slider) * STEP);
}

static void on_output(lv_event_t *e)
{
	struct settings *s = lv_event_get_user_data(e);

	gui_action_post(ACT_SET_AUDIO_OUT, (s->output + 1) % AUDIO_OUT_COUNT);
}
#endif

#if defined(CONFIG_WIFI) && defined(CONFIG_SCREEN_WIFI)
static void on_wifi(lv_event_t *e)
{
	(void)e;
	gui_action_post(ACT_OPEN_SCREEN, SCREEN_WIFI);
}
#endif

static lv_obj_t *row(lv_obj_t *parent, const char *name)
{
	lv_obj_t *r = lv_obj_create(parent), *label;

	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 16, 0);
	lv_obj_set_style_pad_ver(r, ROW_PAD, 0);
	label = lv_label_create(r);
	lv_obj_set_width(label, 140);
	lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
	lv_label_set_text(label, name);
	return r;
}

static lv_obj_t *slider(lv_obj_t *r, lv_event_cb_t cb, lv_obj_t **value)
{
	lv_obj_t *sl = lv_slider_create(r);
	struct ui_frame frame;

	lv_obj_set_width(sl, 180);
	frame = ui_focus_style(sl);
	/* room for the frame around the knob; it may use the row's empty padding */
	lv_obj_set_style_margin_hor(sl, frame.hor, 0);
	lv_obj_set_style_margin_ver(sl, LV_MAX(frame.ver - ROW_PAD, 0), 0);
	lv_slider_set_range(sl, 0, 100 / STEP);
	lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);
	*value = lv_label_create(r);
	lv_obj_set_style_text_font(*value, UI_FONT_MID, 0);
	return sl;
}

static lv_obj_t *button(lv_obj_t *r, lv_event_cb_t cb, void *ud, lv_obj_t **label)
{
	lv_obj_t *btn = lv_button_create(r);

	lv_obj_set_style_pad_ver(btn, 6, 0);
	ui_focus_style(btn);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
	*label = lv_label_create(btn);
	lv_obj_set_style_text_font(*label, UI_FONT_MID, 0);
	return btn;
}

static void *settings_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct settings *s = calloc(1, sizeof(*s));
	lv_obj_t *scr, *body, *r, *hint, *label;

	(void)parent;
	if (!s)
		return NULL;
	s->shown_brightness = -2;
	s->shown_volume = -2;
	s->output = -1;
	scr = lv_obj_create(NULL);
	statusbar_create(scr, LV_SYMBOL_SETTINGS, tr(S_SETTINGS));
	body = screen_body(scr);
	lv_obj_set_style_pad_row(body, 2, 0);

	r = row(body, tr(S_BRIGHTNESS));
	s->slider = slider(r, on_slider, &s->value);
	lv_slider_set_range(s->slider, 1, 100 / STEP);
	s->no_bl = lv_label_create(r);
	lv_obj_set_style_text_color(s->no_bl, UI_COLOR_DIM, 0);
	lv_label_set_text(s->no_bl, tr(S_NOT_AVAILABLE));

#ifdef CONFIG_AUDIO
	s->vol_row = row(body, tr(S_VOLUME));
	s->vol_slider = slider(s->vol_row, on_volume, &s->vol_value);
	s->out_row = row(body, tr(S_OUTPUT));
	button(s->out_row, on_output, s, &s->out_label);
#endif

	if (i18n_count() > 1) {
		r = row(body, tr(S_LANGUAGE));
		button(r, on_lang, NULL, &label);
		lv_label_set_text(label, tr(S_LANG_NAME));
	}

#if defined(CONFIG_WIFI) && defined(CONFIG_SCREEN_WIFI)
	s->wifi_row = row(body, tr(S_WIFI));
	button(s->wifi_row, on_wifi, NULL, &s->wifi_label);
#endif

	hint = lv_label_create(body);
	lv_obj_set_style_text_color(hint, UI_COLOR_DIM, 0);
	lv_label_set_text_fmt(hint, LV_SYMBOL_LEFT " " LV_SYMBOL_RIGHT " %s", tr(S_CHANGE_BACK));
	*root = scr;
	return s;
}

static void update_audio(struct settings *s, const struct gui_ctx *c)
{
	static const enum str_id out_names[AUDIO_OUT_COUNT] = {
		S_OUT_AUTO, S_OUT_SPEAKER, S_OUT_HEADPHONES,
	};
	bool has = c->audio.available && c->audio.volume >= 0;

	lv_obj_set_hidden(s->vol_row, !has);
	lv_obj_set_hidden(s->out_row, !has || !c->audio.can_route);
	if (has && c->audio.volume != s->shown_volume) {
		s->shown_volume = c->audio.volume;
		lv_slider_set_value(s->vol_slider, (c->audio.volume + STEP / 2) / STEP, LV_ANIM_OFF);
		lv_label_set_text_fmt(s->vol_value, "%d%%", c->audio.volume);
	}
	/* "Auto" says where the sound goes now */
	s->output = c->audio.output;
	if (c->audio.output == AUDIO_OUT_AUTO)
		lv_label_set_text_fmt(s->out_label, tr(S_OUT_AUTO_NOW_FMT),
				      tr(c->audio.headphones ? S_NOW_HEADPHONES : S_NOW_SPEAKER));
	else
		lv_label_set_text(s->out_label, tr(out_names[c->audio.output]));
}

static void update_wifi(struct settings *s, const struct gui_ctx *c)
{
	const char *text;

	if (!c->wifi.available)
		text = tr(S_WIFI_UNAVAILABLE);
	else if (!c->wifi.enabled || c->wifi.link == WIFI_OFF)
		text = tr(S_WIFI_OFF);
	else if (c->wifi.link == WIFI_CONNECTED)
		text = c->wifi.ssid;
	else if (c->wifi.link == WIFI_CONNECTING)
		text = "...";
	else
		text = tr(S_WIFI_DISCONNECTED);
	lv_label_set_text_fmt(s->wifi_label, "%s  " LV_SYMBOL_RIGHT, text);
}

static void settings_update(void *self, const struct gui_ctx *c, uint32_t changed)
{
	struct settings *s = self;

	if (changed & CTX_DISPLAY) {
		bool has = c->display.has_backlight && c->display.brightness >= 0;

		lv_obj_set_hidden(s->slider, !has);
		lv_obj_set_hidden(s->no_bl, has);
		if (has && c->display.brightness != s->shown_brightness) {
			s->shown_brightness = c->display.brightness;
			/* does not emit VALUE_CHANGED, so no loop back to control */
			lv_slider_set_value(s->slider, (c->display.brightness + STEP / 2) / STEP,
					    LV_ANIM_OFF);
			lv_label_set_text_fmt(s->value, "%d%%", c->display.brightness);
		}
		if (!has)
			lv_label_set_text(s->value, "");
	}
	if ((changed & CTX_AUDIO) && s->vol_row)
		update_audio(s, c);
	if ((changed & CTX_WIFI) && s->wifi_row)
		update_wifi(s, c);
}

static void settings_deinit(void *self)
{
	free(self);
}

const struct widget_ops settings_screen = {
	.name = "settings",
	.deps = CTX_DISPLAY | CTX_AUDIO | CTX_WIFI,
	.init = settings_init,
	.update = settings_update,
	.deinit = settings_deinit,
};
