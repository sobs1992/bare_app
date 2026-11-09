/*
 * Status bar, the header of every screen (widgets/console.c): the screen's
 * title on the left; volume (CTX_AUDIO), Wi-Fi (CTX_WIFI),
 * battery (CTX_POWER) and the time on the right. The time only once the
 * clock has been set (a board without a set RTC starts in 1970 or 2000).
 */
#include <stdlib.h>
#include <time.h>

#include "../widget.h"

#define CLOCK_VALID 1704067200	/* 2024-01-01: earlier is a clock never set */

struct statusbar {
	lv_obj_t *title, *volume, *wifi, *battery, *clock;
	lv_timer_t *tick;
	int shown_minute;	/* minutes since the epoch, -1 none */
	int shown_volume;	/* percent, -1 hidden, -2 nothing yet */
	int shown_cap;
	int shown_flags;	/* bit 0 charging, 1 online, 2 available, -1 nothing yet */
	int shown_link;		/* enum wifi_link, -1 nothing yet */
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

static void update_clock(struct statusbar *s)
{
	time_t now = time(NULL);
	struct tm tm;
	int minute = now >= CLOCK_VALID ? (int)(now / 60) : -1;

	if (minute == s->shown_minute)
		return;
	s->shown_minute = minute;
	lv_obj_set_hidden(s->clock, minute < 0);
	if (minute >= 0 && localtime_r(&now, &tm))
		lv_label_set_text_fmt(s->clock, "%d:%02d", tm.tm_hour, tm.tm_min);
}

static void on_tick(lv_timer_t *t)
{
	update_clock(lv_timer_get_user_data(t));
}

static void *statusbar_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct statusbar *s = calloc(1, sizeof(*s));
	lv_obj_t *bar, *right;

	if (!s)
		return NULL;
	s->shown_volume = -2;
	s->shown_flags = -1;
	s->shown_link = -1;
	bar = lv_obj_create(parent);
	lv_obj_remove_style_all(bar);
	/* the header rows of the grid, on the screen's background */
	lv_obj_set_size(bar, LV_PCT(100), ui_header_h());
	lv_obj_set_style_pad_hor(bar, ui_margin(), 0);
	lv_obj_set_scrollable(bar, false);

	s->title = lv_label_create(bar);
	lv_obj_set_style_text_font(s->title, UI_FONT_MID, 0);
	lv_obj_align(s->title, LV_ALIGN_LEFT_MID, 0, 0);
	lv_label_set_text(s->title, "");

	right = lv_obj_create(bar);
	lv_obj_remove_style_all(right);
	lv_obj_set_size(right, LV_SIZE_CONTENT, LV_PCT(100));
	lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(right, ui_px(14), 0);
	s->volume = lv_label_create(right);
	lv_obj_set_style_text_font(s->volume, UI_FONT_MID, 0);
	lv_label_set_text(s->volume, "");
	lv_obj_set_hidden(s->volume, true);
	s->wifi = lv_label_create(right);
	lv_obj_set_style_text_font(s->wifi, UI_FONT_MID, 0);
	lv_label_set_text(s->wifi, LV_SYMBOL_WIFI);
	lv_obj_set_hidden(s->wifi, true);
	s->battery = lv_label_create(right);
	lv_obj_set_style_text_font(s->battery, UI_FONT_MID, 0);
	lv_label_set_text(s->battery, "");
	s->clock = lv_label_create(right);
	lv_obj_set_style_text_font(s->clock, UI_FONT_MID, 0);
	lv_label_set_text(s->clock, "");
	s->shown_minute = -2;
	update_clock(s);
	s->tick = lv_timer_create(on_tick, 1000, s);

	*root = bar;
	return s;
}

/* The icon alone (the volume OSD shows the level); muted: dim; no mixer: none */
static void update_volume(struct statusbar *s, const struct gui_ctx *c)
{
	int vol = c->audio.available ? c->audio.volume : -1;

	if (vol == s->shown_volume)
		return;
	s->shown_volume = vol;
	lv_obj_set_hidden(s->volume, vol < 0);
	if (vol < 0)
		return;
	lv_label_set_text(s->volume, ui_volume_symbol(vol));
	lv_obj_set_style_text_color(s->volume, vol == 0 ? UI_COLOR_DIM : lv_color_white(), 0);
}

/* Connected: white; joining or waiting for an address: dim; otherwise none */
static void update_wifi(struct statusbar *s, const struct gui_ctx *c)
{
	int link = c->wifi.link == WIFI_CONNECTED && !c->wifi.ip[0] ? WIFI_CONNECTING : c->wifi.link;

	if (link == s->shown_link)
		return;
	s->shown_link = link;
	lv_obj_set_hidden(s->wifi, link < WIFI_CONNECTING);
	lv_obj_set_style_text_color(s->wifi, link == WIFI_CONNECTED ? lv_color_white() : UI_COLOR_DIM,
				    0);
}

static void statusbar_update(void *self, const struct gui_ctx *c, uint32_t changed)
{
	struct statusbar *s = self;
	int flags = c->power.charging | c->power.online << 1 | c->power.available << 2;

	if (changed & CTX_AUDIO)
		update_volume(s, c);
	if (changed & CTX_WIFI)
		update_wifi(s, c);
	if (!(changed & CTX_POWER) || (flags == s->shown_flags && c->power.capacity == s->shown_cap))
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
	struct statusbar *s = self;

	lv_timer_delete(s->tick);
	free(s);
}

const struct widget_ops statusbar_widget = {
	.name = "statusbar",
	.deps = CTX_POWER | CTX_AUDIO | CTX_WIFI,
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
