/* LVGL view backend: glue between the view core and LVGL. */
#include "lvgl_view.h"

#include "core/log.h"
#include "core/util.h"

#define TAG "lvgl"
#define TOAST_MS 2500

static lv_display_t *disp;
static lv_style_t focus_style;

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

void ui_focus_style(lv_obj_t *obj)
{
	lv_obj_add_style(obj, &focus_style, LV_STATE_FOCUS_KEY);
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
	lv_style_init(&focus_style);
	lv_style_set_outline_color(&focus_style, lv_color_white());
	lv_style_set_outline_width(&focus_style, 3);
	lv_style_set_outline_pad(&focus_style, 3);
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
	char text[64];

	switch (n->kind) {
	case NOTIFY_BATTERY_LOW:
		lv_snprintf(text, sizeof(text), LV_SYMBOL_WARNING "  Battery low: %d%%", n->value);
		break;
	case NOTIFY_CHARGER:
		lv_snprintf(text, sizeof(text), "%s", n->value ? LV_SYMBOL_CHARGE "  Charger connected"
							       : "Charger disconnected");
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
