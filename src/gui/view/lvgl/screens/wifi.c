/*
 * Wi-Fi (CTX_WIFI): on/off, the link, networks in range. A on a network
 * connects (a new secured one asks for the key on the key screen), X twice
 * forgets a saved one. Scans when opened and then now and again.
 *
 * Rows are made once and reused, so a new scan does not rebuild the list;
 * the focus follows the network it was on when the order changes.
 */
#include <stdlib.h>
#include <string.h>

#include "../widget.h"
#include "core/util.h"
#include "gui/action.h"

#define RESCAN_MS 15000
#define FORGET_MS 3000		/* X twice within this time */

struct wifi_screen {
	lv_obj_t *sw, *status, *header, *empty, *hint;
	lv_obj_t *rows[WIFI_MAX_NETS], *names[WIFI_MAX_NETS], *infos[WIFI_MAX_NETS];
	struct wifi_net nets[WIFI_MAX_NETS];	/* what the rows show */
	bool current[WIFI_MAX_NETS];		/* ...and which one is joined */
	enum wifi_link shown_link;
	int count;
	enum wifi_link link;
	char ssid[WIFI_SSID_MAX + 1];		/* of the link */
	lv_timer_t *rescan;
	int forget_row;				/* X pressed once on it, -1 none */
	uint32_t forget_at;
};

static void on_switch(lv_event_t *e)
{
	gui_action_post(ACT_WIFI_ENABLE, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_rescan(lv_timer_t *t)
{
	(void)t;
	gui_action_post(ACT_WIFI_SCAN, 0);
}

static int row_index(struct wifi_screen *w, lv_obj_t *row)
{
	int i;

	for (i = 0; i < WIFI_MAX_NETS; i++)
		if (w->rows[i] == row)
			return i;
	return -1;
}

static bool is_current(const struct wifi_screen *w, const struct wifi_net *n)
{
	return w->link >= WIFI_CONNECTING && !strcmp(n->ssid, w->ssid);
}

static void on_click(lv_event_t *e)
{
	struct wifi_screen *w = lv_event_get_user_data(e);
	int i = row_index(w, lv_event_get_current_target(e));
	const struct wifi_net *n;

	if (i < 0 || i >= w->count)
		return;
	n = &w->nets[i];
	if (is_current(w, n))
		return;		/* already there */
	if (n->saved || !n->secured) {
		gui_action_post_secret(ACT_WIFI_CONNECT, n->ssid, "");
	} else {
		gui_action_post_str(ACT_WIFI_ASK_KEY, n->ssid);
		gui_action_post(ACT_OPEN_SCREEN, SCREEN_WIFI_KEY);
	}
}

static void on_key(lv_event_t *e)
{
	struct wifi_screen *w = lv_event_get_user_data(e);
	int i = row_index(w, lv_event_get_current_target(e));
	char text[96];

	if (lv_event_get_key(e) != UI_LV_KEY_ALT || i < 0 || i >= w->count || !w->nets[i].saved)
		return;
	if (w->forget_row == i && lv_tick_elaps(w->forget_at) < FORGET_MS) {
		gui_action_post_str(ACT_WIFI_FORGET, w->nets[i].ssid);
		w->forget_row = -1;
		return;
	}
	w->forget_row = i;
	w->forget_at = lv_tick_get();
	lv_snprintf(text, sizeof(text), tr(S_WIFI_FORGET_AGAIN_FMT), w->nets[i].ssid);
	ui_toast(text);
}

static void *wifi_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct wifi_screen *w = calloc(1, sizeof(*w));
	lv_obj_t *scr, *body, *r, *label;
	int i;

	(void)parent;
	if (!w)
		return NULL;
	w->forget_row = -1;
	w->link = -1;
	scr = lv_obj_create(NULL);
	statusbar_create(scr, LV_SYMBOL_WIFI, tr(S_WIFI));
	body = screen_body(scr);
	lv_obj_set_style_pad_row(body, 6, 0);

	r = lv_obj_create(body);
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 16, 0);
	w->sw = lv_switch_create(r);
	ui_focus_style(w->sw);
	lv_obj_add_event_cb(w->sw, on_switch, LV_EVENT_VALUE_CHANGED, NULL);
	w->status = lv_label_create(r);
	lv_obj_set_flex_grow(w->status, 1);
	lv_obj_set_style_text_font(w->status, UI_FONT_MID, 0);
	lv_label_set_long_mode(w->status, LV_LABEL_LONG_MODE_DOTS);

	w->header = lv_label_create(body);
	lv_obj_set_width(w->header, LV_PCT(100));
	lv_obj_set_style_text_color(w->header, UI_COLOR_DIM, 0);
	w->empty = lv_label_create(body);
	lv_obj_set_width(w->empty, LV_PCT(100));
	lv_label_set_long_mode(w->empty, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_font(w->empty, UI_FONT_MID, 0);

	for (i = 0; i < WIFI_MAX_NETS; i++) {
		r = w->rows[i] = lv_button_create(body);
		lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_ver(r, 8, 0);
		ui_focus_style(r);
		lv_obj_add_event_cb(r, on_click, LV_EVENT_CLICKED, w);
		lv_obj_add_event_cb(r, on_key, LV_EVENT_KEY, w);
		label = w->names[i] = lv_label_create(r);
		lv_obj_set_flex_grow(label, 1);
		lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
		lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
		w->infos[i] = lv_label_create(r);
		lv_obj_set_hidden(r, true);
	}

	w->hint = lv_label_create(body);
	lv_obj_set_style_text_color(w->hint, UI_COLOR_DIM, 0);
	lv_label_set_text(w->hint, tr(S_WIFI_HINT));

	gui_action_post(ACT_WIFI_SCAN, 0);
	w->rescan = lv_timer_create(on_rescan, RESCAN_MS, NULL);
	*root = scr;
	return w;
}

/* Show net in row i unless it already does */
static void show_row(struct wifi_screen *w, int i, const struct wifi_net *n)
{
	char info[64];

	if (!lv_obj_is_hidden(w->rows[i]) && !memcmp(&w->nets[i], n, sizeof(*n)) &&
	    w->current[i] == is_current(w, n) && (!w->current[i] || w->shown_link == w->link))
		return;
	w->nets[i] = *n;
	w->current[i] = is_current(w, n);
	lv_label_set_text_fmt(w->names[i], "%s%s",
			      is_current(w, n) && w->link == WIFI_CONNECTED ? LV_SYMBOL_OK "  " : "",
			      n->ssid);
	lv_snprintf(info, sizeof(info), "%s%s%d%%", n->saved ? tr(S_SAVED) : !n->secured ? tr(S_OPEN) : "",
		    n->saved || !n->secured ? "   " : "", n->signal);
	lv_label_set_text(w->infos[i], info);
	lv_obj_set_hidden(w->rows[i], false);
}

static void update_list(struct wifi_screen *w, const struct gui_ctx *c)
{
	lv_group_t *g = lv_obj_get_group(w->sw);
	lv_obj_t *focused = g ? lv_group_get_focused(g) : NULL, *want;
	int was = row_index(w, focused), i;
	char focus_ssid[WIFI_SSID_MAX + 1] = "";
	bool on = c->wifi.available && c->wifi.enabled;

	if (was >= 0 && was < w->count)
		memcpy(focus_ssid, w->nets[was].ssid, sizeof(focus_ssid));
	w->count = on ? c->wifi.count : 0;
	for (i = 0; i < WIFI_MAX_NETS; i++) {
		if (i < w->count)
			show_row(w, i, &c->wifi.nets[i]);
		else
			lv_obj_set_hidden(w->rows[i], true);
	}
	w->shown_link = w->link;
	if (was >= 0) {
		/* stay on the same network; if it is gone, on the same place */
		for (i = 0; i < w->count && strcmp(w->nets[i].ssid, focus_ssid); i++)
			;
		if (i == w->count)
			i = MIN(was, w->count - 1);
		want = i >= 0 ? w->rows[i] : w->sw;
		if (want != focused) {
			lv_group_focus_obj(want);
			lv_obj_add_state(want, LV_STATE_FOCUS_KEY);
			w->forget_row = -1;
		}
	}

	lv_obj_set_hidden(w->header, !on);
	lv_label_set_text_fmt(w->header, "%s%s%s", tr(S_NETWORKS), c->wifi.scanning ? ", " : "",
			      c->wifi.scanning ? tr(S_SEARCHING) : "");
	/* off: the status line says so */
	lv_label_set_text(w->empty, tr(c->wifi.available ? S_NO_NETWORKS : S_WIFI_NO_DAEMON));
	lv_obj_set_hidden(w->empty, w->count > 0 || (on && c->wifi.scanning) ||
				    (c->wifi.available && !c->wifi.enabled));
	lv_obj_set_hidden(w->hint, w->count == 0);
}

static void wifi_update(void *self, const struct gui_ctx *c, uint32_t changed)
{
	struct wifi_screen *w = self;

	(void)changed;
	w->link = c->wifi.link;
	memcpy(w->ssid, c->wifi.ssid, sizeof(w->ssid));

	lv_obj_set_state(w->sw, LV_STATE_CHECKED, c->wifi.enabled);
	lv_obj_set_state(w->sw, LV_STATE_DISABLED, !c->wifi.available);
	if (!c->wifi.available || !c->wifi.enabled || c->wifi.link == WIFI_OFF)
		lv_label_set_text(w->status, tr(c->wifi.available ? S_WIFI_OFF : S_WIFI_UNAVAILABLE));
	else if (c->wifi.link == WIFI_CONNECTING)
		lv_label_set_text_fmt(w->status, tr(S_WIFI_CONNECTING_FMT), c->wifi.ssid);
	else if (c->wifi.link == WIFI_CONNECTED && !c->wifi.ip[0])
		lv_label_set_text_fmt(w->status, tr(S_WIFI_NO_IP_FMT), c->wifi.ssid);
	else if (c->wifi.link == WIFI_CONNECTED)
		lv_label_set_text_fmt(w->status, "%s, %s", c->wifi.ssid, c->wifi.ip);
	else
		lv_label_set_text(w->status, tr(S_WIFI_DISCONNECTED));
	update_list(w, c);
}

static void wifi_deinit(void *self)
{
	struct wifi_screen *w = self;

	lv_timer_delete(w->rescan);
	free(w);
}

const struct widget_ops wifi_screen = {
	.name = "wifi",
	.deps = CTX_WIFI,
	.init = wifi_init,
	.update = wifi_update,
	.deinit = wifi_deinit,
};
