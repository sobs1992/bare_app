/*
 * Status bar, the header of every screen (widgets/console.c): the screen's
 * title on the left, the time in the middle; volume (CTX_AUDIO), Wi-Fi
 * (CTX_WIFI) and battery (CTX_POWER) on the right. The time only once the
 * clock has been set (a board without a set RTC starts in 1970 or 2000).
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../widget.h"

#define CLOCK_VALID 1704067200	/* 2024-01-01: earlier is a clock never set */

struct statusbar {
	lv_obj_t *title, *volume, *wifi, *clock;
	lv_obj_t *battery, *bat_bolt, *bat_body, *bat_fill, *bat_nub, *bat_text;
	lv_timer_t *tick;
	int shown_minute;	/* minutes since the epoch, -1 none */
	int shown_volume;	/* percent, -1 hidden, -2 nothing yet */
	int shown_cap;
	int shown_flags;	/* bit 0 charging, 1 online, 2 available, -1 nothing yet */
	int shown_link;		/* enum wifi_link, -1 nothing yet */
};

#define BAT_W 42	/* the battery's pill, in the 480x320 layout */
#define BAT_H 18
#define BAT_EMPTY lv_color_hex(0x2a2e36)	/* the pill where it is not filled: red level */
#define BAT_NUB   lv_color_hex(0x5a5f69)	/* its terminal until full: red level */
#define BAT_INK   lv_color_hex(0x101216)	/* the digits on a lit pill */
#define BAT_TINT  LV_OPA_50	/* how much of the level's colour the empty part has */
#define BAT_RED_OPA LV_OPA_40	/* the red level's fill, see-through */

/* Charging: green; 20 % and less: yellow; 10 % and less: red */
static lv_color_t battery_color(int cap, bool charging)
{
	if (charging)
		return lv_color_hex(0x40d26e);
	if (cap <= 10)
		return lv_color_hex(0xff4040);
	if (cap <= 20)
		return lv_color_hex(0xffd23f);
	return lv_color_white();
}

static lv_obj_t *plain_obj(lv_obj_t *parent)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_set_scrollable(o, false);
	return o;
}

/*
 * A pill filled to the level with the percent inside. The empty part has
 * the fill's colour too, darker, so that one colour of digits reads on both
 * and a digit is never cut in two where the fill ends.
 */
static void battery_create(struct statusbar *s, lv_obj_t *parent)
{
	s->battery = plain_obj(parent);
	lv_obj_set_size(s->battery, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(s->battery, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(s->battery, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(s->battery, LV_MAX(ui_px(1), 1), 0);

	/* only when the level is unknown: a bolt on the charger */
	s->bat_bolt = lv_label_create(s->battery);
	lv_obj_set_style_text_font(s->bat_bolt, UI_FONT_MID, 0);
	lv_label_set_text(s->bat_bolt, LV_SYMBOL_CHARGE);

	s->bat_body = plain_obj(s->battery);
	lv_obj_set_size(s->bat_body, ui_px(BAT_W), ui_px(BAT_H));
	lv_obj_set_style_radius(s->bat_body, ui_px(5), 0);
	lv_obj_set_style_clip_corner(s->bat_body, true, 0);
	lv_obj_set_style_bg_opa(s->bat_body, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(s->bat_body, BAT_EMPTY, 0);

	s->bat_fill = plain_obj(s->bat_body);
	lv_obj_set_height(s->bat_fill, LV_PCT(100));
	lv_obj_set_style_bg_opa(s->bat_fill, LV_OPA_COVER, 0);

	s->bat_text = lv_label_create(s->bat_body);	/* over the fill */
	lv_obj_set_style_text_font(s->bat_text, UI_FONT_SMALL, 0);
	lv_obj_center(s->bat_text);
	lv_label_set_text(s->bat_text, "");

	s->bat_nub = plain_obj(s->battery);
	lv_obj_set_size(s->bat_nub, LV_MAX(ui_px(2), 2), ui_px(7));
	lv_obj_set_style_radius(s->bat_nub, 1, 0);
	lv_obj_set_style_bg_opa(s->bat_nub, LV_OPA_COVER, 0);
	lv_obj_set_hidden(s->battery, true);
}

static void update_battery(struct statusbar *s, const struct gui_ctx *c)
{
	int cap = c->power.capacity;
	bool known = c->power.available && cap >= 0;
	lv_color_t col = battery_color(cap, c->power.charging);
	char text[16];

	lv_obj_set_hidden(s->battery, !known && !c->power.online);
	lv_obj_set_hidden(s->bat_bolt, known);
	lv_obj_set_hidden(s->bat_body, !known);
	lv_obj_set_hidden(s->bat_nub, !known);
	if (!known)
		return;
	snprintf(text, sizeof(text), "%s%d", c->power.charging ? LV_SYMBOL_CHARGE " " : "", cap);
	ui_set_text(s->bat_text, text);
	lv_obj_set_width(s->bat_fill, ui_px(BAT_W) * cap / 100);
	if (!c->power.charging && cap <= 10) {
		/* nearly empty: a dark pill, a faint red fill and red digits */
		lv_obj_set_style_bg_color(s->bat_body, BAT_EMPTY, 0);
		lv_obj_set_style_bg_color(s->bat_fill, lv_color_mix(col, BAT_EMPTY, BAT_RED_OPA), 0);
		lv_obj_set_style_text_color(s->bat_text, col, 0);
		lv_obj_set_style_bg_color(s->bat_nub, BAT_NUB, 0);
		return;
	}
	/* lit: the level's colour, darker where empty; black digits read on both */
	lv_obj_set_style_bg_color(s->bat_body, lv_color_mix(col, BAT_EMPTY, BAT_TINT), 0);
	lv_obj_set_style_bg_color(s->bat_fill, col, 0);
	lv_obj_set_style_text_color(s->bat_text, BAT_INK, 0);
	/* the terminal is the pill's last bit: filled only when full */
	lv_obj_set_style_bg_color(s->bat_nub, cap >= 100 ? col : lv_color_mix(col, BAT_EMPTY, BAT_TINT), 0);
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
	/* a long title (an SSID) stops short of the time in the middle */
	lv_obj_set_style_max_width(s->title, LV_PCT(40), 0);
	lv_label_set_long_mode(s->title, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(s->title, "");

	s->clock = lv_label_create(bar);
	lv_obj_set_style_text_font(s->clock, UI_FONT_MID, 0);
	lv_obj_align(s->clock, LV_ALIGN_CENTER, 0, 0);
	lv_label_set_text(s->clock, "");

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
	battery_create(s, right);
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
	update_battery(s, c);
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
