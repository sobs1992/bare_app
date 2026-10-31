/*
 * Key for a new Wi-Fi network (the one in CTX_WIFI wifi.asked): on-screen
 * keyboard, the D-pad walks its keys, A types, X deletes, the keyboard's
 * tick connects and goes back. The key is shown as typed: on a gamepad
 * every character costs several presses, and mistakes must be visible.
 */
#include <stdlib.h>
#include <string.h>

#include "../widget.h"
#include "gui/action.h"

struct wifi_key {
	lv_obj_t *bar, *ta, *kb;
	char ssid[WIFI_SSID_MAX + 1];
};

static void on_ready(lv_event_t *e)
{
	struct wifi_key *k = lv_event_get_user_data(e);
	const char *key = lv_textarea_get_text(k->ta);
	size_t n = strlen(key);

	if (n < 8 || n > 63) {
		ui_toast(tr(S_WIFI_KEY_SHORT));
		return;
	}
	gui_action_post_secret(ACT_WIFI_CONNECT, k->ssid, key);
	lv_textarea_set_text(k->ta, "");
	gui_action_post(ACT_BACK, 0);
}

static void on_kb_value(lv_event_t *e)
{
	lv_obj_t *kb = lv_event_get_target(e);
	const char *txt = lv_buttonmatrix_get_button_text(kb, lv_buttonmatrix_get_selected_button(kb));

	if (txt && !strcmp(txt, LV_SYMBOL_KEYBOARD))
		gui_action_post(ACT_BACK, 0);	/* the "hide keyboard" key: leave */
}

static void on_kb_key(lv_event_t *e)
{
	struct wifi_key *k = lv_event_get_user_data(e);

	if (lv_event_get_key(e) == UI_LV_KEY_ALT)
		lv_textarea_delete_char(k->ta);
}

static void *wifi_key_init(lv_obj_t *parent, lv_obj_t **root)
{
	struct wifi_key *k = calloc(1, sizeof(*k));
	lv_obj_t *scr, *hint;

	(void)parent;
	if (!k)
		return NULL;
	scr = lv_obj_create(NULL);
	lv_obj_set_scrollable(scr, false);
	k->bar = statusbar_create(scr, LV_SYMBOL_WIFI, "");

	k->ta = lv_textarea_create(scr);
	lv_group_remove_obj(k->ta);	/* the keyboard has the focus */
	lv_textarea_set_one_line(k->ta, true);
	lv_textarea_set_max_length(k->ta, 63);
	lv_textarea_set_placeholder_text(k->ta, tr(S_WIFI_KEY));
	lv_obj_set_style_text_font(k->ta, UI_FONT_MID, 0);
	lv_obj_set_width(k->ta, lv_display_get_horizontal_resolution(lv_display_get_default()) - 20);
	lv_obj_align(k->ta, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H + 8);
	lv_obj_add_state(k->ta, LV_STATE_FOCUSED);	/* shows the cursor */

	hint = lv_label_create(scr);
	lv_obj_set_style_text_color(hint, UI_COLOR_DIM, 0);
	lv_label_set_text_fmt(hint, tr(S_WIFI_KEY_HINT_FMT), LV_SYMBOL_OK);
	lv_obj_align_to(hint, k->ta, LV_ALIGN_OUT_BOTTOM_LEFT, 4, 6);

	k->kb = lv_keyboard_create(scr);
	lv_obj_set_style_text_font(k->kb, UI_FONT_MID, 0);
	lv_obj_set_size(k->kb, LV_PCT(100), LV_PCT(55));
	lv_obj_align(k->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
	ui_focus_style(k->kb);
	lv_keyboard_set_textarea(k->kb, k->ta);
	lv_obj_add_event_cb(k->kb, on_ready, LV_EVENT_READY, k);
	lv_obj_add_event_cb(k->kb, on_kb_value, LV_EVENT_VALUE_CHANGED, k);
	lv_obj_add_event_cb(k->kb, on_kb_key, LV_EVENT_KEY, k);
	*root = scr;
	return k;
}

static void wifi_key_update(void *self, const struct gui_ctx *c, uint32_t changed)
{
	struct wifi_key *k = self;

	(void)changed;
	if (!strcmp(k->ssid, c->wifi.asked))
		return;
	memcpy(k->ssid, c->wifi.asked, sizeof(k->ssid));
	statusbar_set_title(k->bar, LV_SYMBOL_WIFI, k->ssid);
}

static void wifi_key_deinit(void *self)
{
	free(self);
}

const struct widget_ops wifi_key_screen = {
	.name = "wifi-key",
	.deps = CTX_WIFI,
	.init = wifi_key_init,
	.update = wifi_key_update,
	.deinit = wifi_key_deinit,
};
