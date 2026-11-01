/* LVGL view backend: glue between the view core and LVGL. */
#include <string.h>

#include "lvgl_view.h"

#include "core/log.h"
#include "core/util.h"

#define TAG "lvgl"
#define TOAST_MS 2500
#define OSD_MS 1500
#define FOCUS_WIDTH 3
#define FOCUS_PAD 3

static lv_display_t *disp;
static lv_style_t focus_style;
static struct {
	lv_obj_t *box, *icon, *bar, *value;
	lv_timer_t *timer;
} osd;
static lv_obj_t *hold_box;

static uint32_t tick(void)
{
	return (uint32_t)now_ms();
}

static void log_cb(lv_log_level_t level, const char *buf)
{
	LOGI(TAG, "%s%s", level >= LV_LOG_LEVEL_WARN ? "! " : "", buf);
}

static void toast_done(lv_timer_t *t)
{
	lv_obj_delete(lv_timer_get_user_data(t));
}

void ui_toast(const char *text)
{
	lv_obj_t *box = lv_label_create(lv_layer_top());
	lv_timer_t *t;

	lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
	lv_obj_set_style_bg_color(box, lv_color_hex(0x30343c), 0);
	lv_obj_set_style_radius(box, 8, 0);
	lv_obj_set_style_pad_all(box, 10, 0);
	lv_obj_set_style_text_font(box, UI_FONT_MID, 0);
	lv_label_set_text(box, text);
	lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -12);
	t = lv_timer_create(toast_done, TOAST_MS, box);
	lv_timer_set_repeat_count(t, 1);
}

static void osd_done(lv_timer_t *t)
{
	(void)t;
	lv_obj_delete(osd.box);
	memset(&osd, 0, sizeof(osd));	/* the one-shot timer deletes itself */
}

const char *ui_volume_symbol(int percent)
{
	return percent == 0 ? LV_SYMBOL_MUTE : percent < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX;
}

void ui_volume_osd(int percent)
{
	if (!osd.box) {
		osd.box = lv_obj_create(lv_layer_top());
		lv_obj_remove_style_all(osd.box);
		lv_obj_set_size(osd.box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(osd.box, LV_OPA_90, 0);
		lv_obj_set_style_bg_color(osd.box, lv_color_hex(0x30343c), 0);
		lv_obj_set_style_radius(osd.box, 8, 0);
		lv_obj_set_style_pad_all(osd.box, 10, 0);
		lv_obj_set_style_pad_column(osd.box, 12, 0);
		lv_obj_set_flex_flow(osd.box, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(osd.box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		osd.icon = lv_label_create(osd.box);
		lv_obj_set_style_text_font(osd.icon, UI_FONT_MID, 0);
		osd.bar = lv_bar_create(osd.box);
		lv_obj_set_size(osd.bar, 180, 10);
		osd.value = lv_label_create(osd.box);
		lv_obj_set_style_text_font(osd.value, UI_FONT_MID, 0);
		lv_obj_set_width(osd.value, 56);
		lv_obj_align(osd.box, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_H + 8);
		osd.timer = lv_timer_create(osd_done, OSD_MS, NULL);
		lv_timer_set_repeat_count(osd.timer, 1);
	} else {
		lv_timer_reset(osd.timer);
	}
	lv_label_set_text(osd.icon, ui_volume_symbol(percent));
	lv_bar_set_value(osd.bar, percent, LV_ANIM_OFF);
	lv_label_set_text_fmt(osd.value, "%d%%", percent);
}

/* The quit combo or the power key is held: the bar fills up until it acts;
 * ms 0: let go, or done */
static void hold(enum str_id text, int ms)
{
	lv_obj_t *label, *bar;

	if (hold_box)
		lv_obj_delete(hold_box);
	hold_box = NULL;
	if (ms <= 0)
		return;
	hold_box = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(hold_box);
	lv_obj_set_size(hold_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(hold_box, LV_OPA_90, 0);
	lv_obj_set_style_bg_color(hold_box, lv_color_hex(0x30343c), 0);
	lv_obj_set_style_text_color(hold_box, lv_color_white(), 0);
	lv_obj_set_style_radius(hold_box, 8, 0);
	lv_obj_set_style_pad_all(hold_box, 14, 0);
	lv_obj_set_style_pad_row(hold_box, 12, 0);
	lv_obj_set_flex_flow(hold_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(hold_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	label = lv_label_create(hold_box);
	lv_obj_set_style_text_font(label, UI_FONT_MID, 0);
	lv_label_set_text_fmt(label, LV_SYMBOL_POWER "  %s", tr(text));
	bar = lv_bar_create(hold_box);
	lv_obj_set_size(bar, 240, 10);
	lv_obj_set_style_anim_duration(bar, ms, 0);
	lv_bar_set_value(bar, 100, LV_ANIM_ON);
	lv_obj_center(hold_box);
}

/*
 * A slider's knob is centred on the end of the track and sticks out by its
 * radius sideways, but only by its padding up and down. An outline has one
 * distance for all sides, so sliders draw their frame themselves: wide
 * enough for the knob, as low as a button's.
 */
static struct ui_frame slider_frame(lv_obj_t *obj)
{
	int32_t knob = lv_obj_get_style_pad_top(obj, LV_PART_KNOB);

	return (struct ui_frame){
		.hor = lv_obj_get_style_height(obj, LV_PART_MAIN) / 2 + knob + FOCUS_PAD + FOCUS_WIDTH,
		.ver = knob + FOCUS_PAD + FOCUS_WIDTH,
	};
}

static void on_slider_draw(lv_event_t *e)
{
	lv_obj_t *obj = lv_event_get_current_target(e);
	struct ui_frame f = slider_frame(obj);
	lv_draw_rect_dsc_t dsc;
	lv_area_t a;

	if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
		lv_event_set_ext_draw_size(e, LV_MAX(f.hor, f.ver));
		return;
	}
	if (!lv_obj_has_state(obj, LV_STATE_FOCUS_KEY))
		return;
	lv_obj_get_coords(obj, &a);
	a.x1 -= f.hor;
	a.x2 += f.hor;
	a.y1 -= f.ver;
	a.y2 += f.ver;
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_opa = LV_OPA_TRANSP;
	dsc.border_color = lv_color_white();
	dsc.border_width = FOCUS_WIDTH;
	dsc.radius = LV_RADIUS_CIRCLE;
	lv_draw_rect(lv_event_get_layer(e), &dsc, &a);
}

struct ui_frame ui_focus_style(lv_obj_t *obj)
{
	if (lv_obj_has_class(obj, &lv_slider_class)) {
		lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUS_KEY);	/* the theme's */
		lv_obj_add_event_cb(obj, on_slider_draw, LV_EVENT_DRAW_POST, NULL);
		lv_obj_add_event_cb(obj, on_slider_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
		lv_obj_refresh_ext_draw_size(obj);
		return slider_frame(obj);
	}
	lv_obj_add_style(obj, &focus_style, LV_STATE_FOCUS_KEY);
	return (struct ui_frame){ FOCUS_PAD + FOCUS_WIDTH, FOCUS_PAD + FOCUS_WIDTH };
}

static int backend_init(struct hal_display *display, struct mainloop *ml)
{
	(void)ml;
	if (!display) {
		LOGE(TAG, "needs a display");
		return -1;
	}
	lv_init();
	lv_tick_set_cb(tick);
	lv_log_register_print_cb(log_cb);
	disp = port_display_init(display);
	if (!disp)
		return -1;
	lv_display_set_theme(disp, lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
							 lv_palette_main(LV_PALETTE_ORANGE),
							 true, UI_FONT_SMALL));
	/* The theme makes the bottom layer opaque. With alpha it has to let the
	 * video through wherever a screen is transparent. */
	if (lv_color_format_has_alpha(lv_display_get_color_format(disp)))
		lv_obj_set_style_bg_opa(lv_display_get_layer_bottom(disp), LV_OPA_TRANSP, 0);
	lv_style_init(&focus_style);
	lv_style_set_outline_color(&focus_style, lv_color_white());
	lv_style_set_outline_width(&focus_style, FOCUS_WIDTH);
	lv_style_set_outline_pad(&focus_style, FOCUS_PAD);
	if (port_input_init() || screens_init())
		return -1;
	return 0;
}

static bool backend_key(enum ui_key key, bool pressed, bool repeat)
{
	return port_input_key(key, pressed, repeat);
}

static void backend_notify(const struct gui_notify *n)
{
	char text[96];

	switch (n->kind) {
	case NOTIFY_BATTERY_LOW:
		lv_snprintf(text, sizeof(text), "%s  ", LV_SYMBOL_WARNING);
		lv_snprintf(text + strlen(text), sizeof(text) - strlen(text), tr(S_BATTERY_LOW_FMT),
			    n->value);
		break;
	case NOTIFY_CHARGER:
		if (n->value)
			lv_snprintf(text, sizeof(text), LV_SYMBOL_CHARGE "  %s", tr(S_CHARGER_CONNECTED));
		else
			lv_snprintf(text, sizeof(text), "%s", tr(S_CHARGER_DISCONNECTED));
		break;
	case NOTIFY_VOLUME:
		ui_volume_osd(n->value);
		return;
	case NOTIFY_QUIT_HOLD:
		hold(S_QUIT_HOLD, n->value);
		return;
	case NOTIFY_POWER_HOLD:
		hold(S_POWER_HOLD, n->value);
		return;
	case NOTIFY_GAME_QUIT_HOLD:
		hold(S_GAME_QUIT_HOLD, n->value);
		return;
	case NOTIFY_WIFI_CONNECTED:
		lv_snprintf(text, sizeof(text), LV_SYMBOL_WIFI "  %s", tr(S_WIFI_JOINED));
		break;
	case NOTIFY_WIFI_WRONG_KEY:
		lv_snprintf(text, sizeof(text), LV_SYMBOL_WARNING "  %s", tr(S_WIFI_WRONG_KEY));
		break;
	case NOTIFY_WIFI_FAILED:
		lv_snprintf(text, sizeof(text), LV_SYMBOL_WARNING "  %s", tr(S_WIFI_FAILED));
		break;
	default:
		return;
	}
	ui_toast(text);
}

static int backend_process(void)
{
	uint32_t next = lv_timer_handler();

	return next == LV_NO_TIMER_READY ? -1 : (int)next;
}

static void backend_deinit(void)
{
	screens_deinit();
	port_input_deinit();
	lv_deinit();
	memset(&osd, 0, sizeof(osd));
	lv_style_reset(&focus_style);
	disp = NULL;
}

const struct view_backend_ops view_backend_lvgl = {
	.name = "lvgl",
	.init = backend_init,
	.on_key = backend_key,
	.on_notify = backend_notify,
	.process = backend_process,
	.deinit = backend_deinit,
};
