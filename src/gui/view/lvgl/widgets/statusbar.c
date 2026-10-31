/* Status bar: screen title on the left, battery on the right (CTX_POWER). */
#include <stdlib.h>

#include "../widget.h"

struct statusbar {
	lv_obj_t *title, *battery;
	int shown_cap;
	int shown_flags;	/* bit 0 charging, 1 online, 2 available, -1 nothing yet */
};

static const char *battery_symbol(int cap)
{
	if (cap >= 90)
		return LV_SYMBOL_BATTERY_FULL;
	if (cap >= 65)
		return LV_SYMBOL_BATTERY_3;
	if (cap >= 40)
		return LV_SYMBOL_BATTERY_2;
	if (cap >= 15)
		return LV_SYMBOL_BATTERY_1;
	return LV_SYMBOL_BATTERY_EMPTY;
}

static void *statusbar_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct statusbar *s = calloc(1, sizeof(*s));
	lv_obj_t *bar;

	if (!s)
		return NULL;
	s->shown_flags = -1;
	bar = lv_obj_create(parent);
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, LV_PCT(100), UI_STATUSBAR_H);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(0x1b1f27), 0);
	lv_obj_set_style_pad_hor(bar, 12, 0);
	lv_obj_set_scrollable(bar, false);

	s->title = lv_label_create(bar);
	lv_obj_set_style_text_font(s->title, UI_FONT_MID, 0);
	lv_obj_align(s->title, LV_ALIGN_LEFT_MID, 0, 0);
	lv_label_set_text(s->title, "");

	s->battery = lv_label_create(bar);
	lv_obj_set_style_text_font(s->battery, UI_FONT_MID, 0);
	lv_obj_align(s->battery, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_label_set_text(s->battery, "");

	*root = bar;
	return s;
}

static void statusbar_update(void *self, const struct gui_ctx *c, uint32_t changed)
{
	struct statusbar *s = self;
	int flags = c->power.charging | c->power.online << 1 | c->power.available << 2;

	(void)changed;
	if (flags == s->shown_flags && c->power.capacity == s->shown_cap)
		return;
	s->shown_flags = flags;
	s->shown_cap = c->power.capacity;
	if (!c->power.available || c->power.capacity < 0) {
		lv_label_set_text(s->battery, c->power.online ? LV_SYMBOL_CHARGE : "");
		return;
	}
	lv_label_set_text_fmt(s->battery, "%s%d%% %s", c->power.charging ? LV_SYMBOL_CHARGE " " : "",
			      c->power.capacity, battery_symbol(c->power.capacity));
	lv_obj_set_style_text_color(s->battery,
				    c->power.capacity <= 15 && !c->power.online ? UI_COLOR_WARN
										: lv_color_white(), 0);
}

static void statusbar_deinit(void *self)
{
	free(self);
}

const struct widget_ops statusbar_widget = {
	.name = "statusbar",
	.deps = CTX_POWER,
	.init = statusbar_init,
	.update = statusbar_update,
	.deinit = statusbar_deinit,
};

void statusbar_set_title(lv_obj_t *bar, const char *symbol, const char *title)
{
	struct statusbar *s = widget_self(bar);

	if (s && symbol)
		lv_label_set_text_fmt(s->title, "%s  %s", symbol, title);
	else if (s)
		lv_label_set_text(s->title, title);
}

lv_obj_t *statusbar_create(lv_obj_t *parent, const char *symbol, const char *title)
{
	lv_obj_t *bar = widget_create(&statusbar_widget, parent);

	if (bar)
		statusbar_set_title(bar, symbol, title);
	return bar;
}
