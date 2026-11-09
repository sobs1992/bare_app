/*
 * What the console-like screens (home, settings) share: their background,
 * one margin at the sides for everything on them, the header (the status
 * bar on the background) and the bottom: a thin line and the hints of the
 * buttons at its right, each a round badge with the button's letter (a
 * wider one for arrows) and what it does.
 */
#include <stdarg.h>

#include "../widget.h"

#define FOOTER_H_MAX 40
#define BADGE 22

int32_t ui_margin(void)
{
	return lv_display_get_horizontal_resolution(lv_display_get_default()) >= 640 ? 44 : 16;
}

lv_obj_t *console_header(lv_obj_t *screen, const char *symbol, const char *title, int32_t h)
{
	lv_obj_t *bar = statusbar_create(screen, symbol, title);

	if (bar) {
		lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
		lv_obj_set_style_pad_hor(bar, ui_margin(), 0);
		lv_obj_set_height(bar, h);
	}
	return bar;
}

int32_t footer_height(void)
{
	return LV_MIN(FOOTER_H_MAX, lv_display_get_vertical_resolution(lv_display_get_default()) / 9);
}

void screen_console_style(lv_obj_t *screen)
{
	lv_obj_set_style_bg_color(screen, UI_COLOR_BG, 0);
	lv_obj_set_style_bg_grad_color(screen, UI_COLOR_BG2, 0);
	lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_text_color(screen, lv_color_white(), 0);
	lv_obj_set_scrollable(screen, false);
}

lv_obj_t *footer_create(lv_obj_t *screen)
{
	lv_display_t *disp = lv_display_get_default();
	int32_t w = lv_display_get_horizontal_resolution(disp);
	int32_t h = lv_display_get_vertical_resolution(disp), fh = footer_height();
	lv_obj_t *line = lv_obj_create(screen), *box;

	lv_obj_remove_style_all(line);
	lv_obj_set_size(line, w - 2 * ui_margin(), 1);
	lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(line, UI_COLOR_LINE, 0);
	lv_obj_set_pos(line, ui_margin(), h - fh);

	box = lv_obj_create(screen);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, LV_SIZE_CONTENT, fh);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(box, 8, 0);
	lv_obj_align(box, LV_ALIGN_BOTTOM_RIGHT, -ui_margin(), 0);
	return box;
}

void footer_set(lv_obj_t *footer, ...)
{
	const char *button;
	va_list ap;
	bool first = true;

	lv_obj_clean(footer);
	va_start(ap, footer);
	while ((button = va_arg(ap, const char *))) {
		const char *text = va_arg(ap, const char *);
		lv_obj_t *badge = lv_label_create(footer), *label;

		lv_obj_set_height(badge, BADGE);
		lv_obj_set_style_min_width(badge, BADGE, 0);
		lv_obj_set_style_pad_hor(badge, button[1] ? 6 : 0, 0);	/* arrows: a pill */
		lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
		lv_obj_set_style_text_color(badge, UI_COLOR_BG, 0);
		lv_obj_set_style_text_font(badge, UI_FONT_SMALL, 0);
		lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_top(badge, 2, 0);
		if (!first)
			lv_obj_set_style_margin_left(badge, 12, 0);
		lv_label_set_text(badge, button);

		label = lv_label_create(footer);
		lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
		lv_label_set_text(label, text);
		first = false;
	}
	va_end(ap);
}
