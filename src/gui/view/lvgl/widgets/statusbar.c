/*
 * Status bar, the header of every screen (widgets/console.c): the screen's
 * title on the left, the time in the middle; on the right one USB icon
 * while a keyboard, a mouse or the motion sensor is plugged in (CTX_INPUT),
 * volume (CTX_AUDIO), Wi-Fi (CTX_WIFI) and battery (CTX_POWER). Should a
 * long title or the icons reach the time (a narrow screen) it moves right of
 * the title or left of the icons, out of the middle; the title stops short
 * of what is right of it (cut with dots). The time only once the clock has
 * been set (a board without a set RTC starts in 1970 or 2000).
 */
#include <stdlib.h>
#include <time.h>

#include "../widget.h"

#define CLOCK_VALID 1704067200	/* 2024-01-01: earlier is a clock never set */
#define GAP_PX 8		/* between the icons on the right (ui_px()) */

struct statusbar {
	lv_obj_t *title, *devices, *volume, *wifi, *clock;
	lv_obj_t *battery, *bat_bolt, *bat_icon;
	lv_timer_t *tick;
	int shown_minute;	/* minutes since the epoch, -1 none */
	int shown_volume;	/* percent, -1 hidden, -2 nothing yet */
	int shown_cap;
	int shown_flags;	/* bit 0 charging, 1 online, 2 available, -1 nothing yet */
	int shown_link;		/* enum wifi_link, -1 nothing yet */
	int shown_devices;	/* INPUT_DEV_*, -1 nothing yet */
	lv_obj_t *right;	/* the icons */
	int32_t fit_right, fit_clock_w, fit_title_w;	/* fit() placed them for these */
};

/* The title, the time and the icons in a row (the file's comment) */
static void fit(struct statusbar *s)
{
	lv_obj_t *bar = lv_obj_get_parent(s->clock);
	int32_t gap = ui_px(GAP_PX), right, clock_w, title_w, x1, limit;
	lv_area_t area, box;
	lv_point_t size;

	lv_obj_update_layout(bar);
	lv_obj_get_coords(s->right, &area);
	right = area.x1;
	clock_w = lv_obj_is_hidden(s->clock) ? -1 : lv_obj_get_width(s->clock);
	lv_text_get_size(&size, lv_label_get_text(s->title), lv_obj_get_style_text_font(s->title, 0),
			 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	title_w = size.x;
	/* nothing moved: leave them (aligning again redraws) */
	if (right == s->fit_right && clock_w == s->fit_clock_w && title_w == s->fit_title_w)
		return;
	s->fit_right = right;
	s->fit_clock_w = clock_w;
	s->fit_title_w = title_w;
	lv_obj_get_content_coords(bar, &box);
	limit = right;
	if (clock_w >= 0) {
		x1 = box.x1 + (lv_area_get_width(&box) - clock_w) / 2;
		x1 = LV_MAX(x1, box.x1 + title_w + gap);	/* right of the title */
		x1 = LV_MIN(x1, right - gap - clock_w);		/* left of the icons */
		lv_obj_align(s->clock, LV_ALIGN_LEFT_MID, x1 - box.x1, 0);
		limit = x1;
	}
	lv_obj_set_style_max_width(s->title, LV_MAX(limit - gap - box.x1, 0), 0);
}

/* The icon (widgets/battery.c); with the level unknown, a bolt on the charger */
static void battery_create(struct statusbar *s, lv_obj_t *parent)
{
	s->battery = lv_obj_create(parent);
	lv_obj_remove_style_all(s->battery);
	lv_obj_set_size(s->battery, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_scrollable(s->battery, false);
	s->bat_bolt = lv_label_create(s->battery);
	lv_obj_set_style_text_font(s->bat_bolt, UI_FONT_MID, 0);
	lv_label_set_text(s->bat_bolt, LV_SYMBOL_CHARGE);
	lv_obj_center(s->bat_bolt);
	s->bat_icon = battery_icon_create(s->battery);
	lv_obj_center(s->bat_icon);
	lv_obj_set_hidden(s->battery, true);
}

static void update_battery(struct statusbar *s, const struct gui_ctx *c)
{
	bool known = c->power.available && c->power.capacity >= 0;

	lv_obj_set_hidden(s->battery, !known && !c->power.online);
	lv_obj_set_hidden(s->bat_bolt, known);
	lv_obj_set_hidden(s->bat_icon, !known);
	if (known)
		battery_icon_set(s->bat_icon, c->power.capacity, c->power.charging);
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
	fit(s);
}

static void on_tick(lv_timer_t *t)
{
	update_clock(lv_timer_get_user_data(t));
}

static lv_obj_t *icon_new(lv_obj_t *parent, const char *symbol)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, UI_FONT_MID, 0);
	lv_label_set_text(l, symbol);
	lv_obj_set_hidden(l, true);
	return l;
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
	s->shown_devices = -1;
	s->fit_right = s->fit_clock_w = s->fit_title_w = -2;	/* nothing placed yet */
	bar = lv_obj_create(parent);
	lv_obj_remove_style_all(bar);
	/* the header rows of the grid, on the screen's background */
	lv_obj_set_size(bar, LV_PCT(100), ui_header_h());
	lv_obj_set_style_pad_hor(bar, ui_margin(), 0);
	lv_obj_set_scrollable(bar, false);

	s->title = lv_label_create(bar);
	lv_obj_set_style_text_font(s->title, UI_FONT_MID, 0);
	lv_obj_align(s->title, LV_ALIGN_LEFT_MID, 0, 0);
	/* fit() sets how far a long title (an SSID) goes */
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
	s->right = right;
	lv_obj_set_style_pad_column(right, ui_px(GAP_PX), 0);
	s->devices = icon_new(right, LV_SYMBOL_USB);
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

/* One USB icon while a keyboard, a mouse or the motion sensor is plugged in */
static void update_devices(struct statusbar *s, const struct gui_ctx *c)
{
	int d = (int)c->input.devices;

	if (d == s->shown_devices)
		return;
	s->shown_devices = d;
	lv_obj_set_hidden(s->devices, !d);
}

static void statusbar_update(void *self, const struct gui_ctx *c, uint32_t changed)
{
	struct statusbar *s = self;
	int flags = c->power.charging | c->power.online << 1 | c->power.available << 2;

	if (changed & CTX_INPUT)
		update_devices(s, c);
	if (changed & CTX_AUDIO)
		update_volume(s, c);
	if (changed & CTX_WIFI)
		update_wifi(s, c);
	if ((changed & CTX_POWER) && (flags != s->shown_flags || c->power.capacity != s->shown_cap)) {
		s->shown_flags = flags;
		s->shown_cap = c->power.capacity;
		update_battery(s, c);
	}
	fit(s);	/* the icons may have grown towards it */
}

static void statusbar_deinit(void *self)
{
	struct statusbar *s = self;

	lv_timer_delete(s->tick);
	free(s);
}

const struct widget_ops statusbar_widget = {
	.name = "statusbar",
	.deps = CTX_POWER | CTX_AUDIO | CTX_WIFI | CTX_INPUT,
	.init = statusbar_init,
	.update = statusbar_update,
	.deinit = statusbar_deinit,
};

void statusbar_set_title(lv_obj_t *bar, const char *symbol, const char *title)
{
	struct statusbar *s = widget_self(bar);

	if (!s)
		return;
	if (symbol)
		lv_label_set_text_fmt(s->title, "%s  %s", symbol, title);
	else
		lv_label_set_text(s->title, title);
	fit(s);
}

lv_obj_t *statusbar_create(lv_obj_t *parent, const char *symbol, const char *title)
{
	lv_obj_t *bar = widget_create(&statusbar_widget, parent);

	if (bar)
		statusbar_set_title(bar, symbol, title);
	return bar;
}
