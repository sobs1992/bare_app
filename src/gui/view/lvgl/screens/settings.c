/* Settings: brightness (CTX_DISPLAY) and battery details (CTX_POWER). */
#include <stdlib.h>

#include "../widget.h"
#include "gui/action.h"

#define STEP 5		/* percent per slider step */

struct settings {
	lv_obj_t *slider, *value, *no_bl, *battery;
	int shown_brightness;
};

static void on_slider(lv_event_t *e)
{
	lv_obj_t *slider = lv_event_get_target(e);

	gui_action_post(ACT_SET_BRIGHTNESS, (int)lv_slider_get_value(slider) * STEP);
}

static lv_obj_t *row(lv_obj_t *parent, const char *name)
{
	lv_obj_t *r = lv_obj_create(parent), *label;

	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 16, 0);
	lv_obj_set_style_pad_ver(r, 6, 0);
	label = lv_label_create(r);
	lv_obj_set_width(label, 120);
	lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
	lv_label_set_text(label, name);
	return r;
}

static void *settings_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct settings *s = calloc(1, sizeof(*s));
	lv_obj_t *scr, *body, *r, *hint;

	(void)parent;
	if (!s)
		return NULL;
	s->shown_brightness = -2;
	scr = lv_obj_create(NULL);
	statusbar_create(scr, LV_SYMBOL_SETTINGS "  Settings");
	body = screen_body(scr);

	r = row(body, "Brightness");
	s->slider = lv_slider_create(r);
	lv_obj_set_width(s->slider, 200);
	ui_focus_style(s->slider);
	lv_slider_set_range(s->slider, 1, 100 / STEP);
	lv_obj_add_event_cb(s->slider, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
	s->value = lv_label_create(r);
	lv_obj_set_style_text_font(s->value, UI_FONT_MID, 0);
	s->no_bl = lv_label_create(r);
	lv_obj_set_style_text_color(s->no_bl, UI_COLOR_DIM, 0);
	lv_label_set_text(s->no_bl, "not available");

	r = row(body, "Battery");
	s->battery = lv_label_create(r);
	lv_obj_set_style_text_font(s->battery, UI_FONT_MID, 0);

	hint = lv_label_create(body);
	lv_obj_set_style_text_color(hint, UI_COLOR_DIM, 0);
	lv_label_set_text(hint, LV_SYMBOL_LEFT " " LV_SYMBOL_RIGHT " change    B  back");
	*root = scr;
	return s;
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
	if (changed & CTX_POWER) {
		if (!c->power.available || c->power.capacity < 0)
			lv_label_set_text(s->battery, c->power.available ? "unknown" : "none");
		else
			lv_label_set_text_fmt(s->battery, "%d%%, %s%s", c->power.capacity,
					      c->power.full ? "full" :
					      c->power.charging ? "charging" : "discharging",
					      c->power.online ? ", charger on" : "");
	}
}

static void settings_deinit(void *self)
{
	free(self);
}

const struct widget_ops settings_screen = {
	.name = "settings",
	.deps = CTX_DISPLAY | CTX_POWER,
	.init = settings_init,
	.update = settings_update,
	.deinit = settings_deinit,
};
