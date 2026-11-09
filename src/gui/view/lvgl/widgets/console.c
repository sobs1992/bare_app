/*
 * What every screen shares, laid out like a console's menus.
 *
 * The grid: the screen is UI_ROWS square cells high; as many whole cells
 * as fit go across, and what is left over is split between the two sides,
 * so the margins (one cell and that) are the same left and right. The
 * header is the top two rows, the footer the bottom two; between them is
 * the body. Sizes that are not whole cells scale with the cell: ui_px()
 * gives a size of the 480x320 layout (a cell of 20) on this screen.
 *
 * The fonts follow the cell: the small and the middle size are 14 and 20
 * pixels on a 320 lines high screen, 20 and 28 from 26 pixel cells (the
 * GO Ultra's 480 lines) up.
 *
 * Then the screen itself: its background, the header (the status bar on
 * the background, its ends at the margins), a thin line under it, the
 * body, and at the bottom a thin line with the hints of the buttons at
 * its right, each a round badge with the button's letter (a wider one for
 * arrows) and what it does.
 */
#include <stdarg.h>
#include <string.h>

#include "../widget.h"

#define BASE_CELL 20		/* the cell of the 480x320 layout */
#define BIG_CELL 26		/* from it on the bigger fonts */

static lv_display_t *disp(void)
{
	return lv_display_get_default();
}

static int32_t screen_w(void)
{
	return lv_display_get_horizontal_resolution(disp());
}

static int32_t screen_h(void)
{
	return lv_display_get_vertical_resolution(disp());
}

int32_t ui_cell(void)
{
	int32_t c = screen_h() / UI_ROWS;

	return c > 0 ? c : 1;
}

int32_t ui_px(int32_t px)
{
	return (px * ui_cell() + BASE_CELL / 2) / BASE_CELL;
}

int32_t ui_margin(void)
{
	int32_t c = ui_cell();

	return c + (screen_w() - screen_w() / c * c) / 2;
}

int32_t ui_header_h(void)
{
	return 2 * ui_cell();
}

int32_t footer_height(void)
{
	return 2 * ui_cell();
}

void ui_body_area(lv_area_t *a)
{
	a->x1 = ui_margin();
	a->x2 = screen_w() - ui_margin() - 1;
	a->y1 = ui_header_h() + ui_cell() / 2;
	a->y2 = screen_h() - footer_height() - ui_cell() / 2 - 1;
}

const lv_font_t *ui_font(enum ui_text size)
{
	bool big = ui_cell() >= BIG_CELL;

#ifdef CONFIG_UI_FONT_CYRILLIC
	if (size == UI_TEXT_COMPACT)
		return &ui_font_14;
	if (size == UI_TEXT_SMALL)
		return big ? &ui_font_20 : &ui_font_14;
	return big ? &ui_font_28 : &ui_font_20;
#else
	if (size == UI_TEXT_COMPACT)
		return &lv_font_montserrat_14;
	if (size == UI_TEXT_SMALL)
		return big ? &lv_font_montserrat_20 : &lv_font_montserrat_14;
	return big ? &lv_font_montserrat_28 : &lv_font_montserrat_20;
#endif
}

void console_focus_box(lv_obj_t *o)
{
	lv_obj_set_style_radius(o, ui_px(8), 0);
	lv_obj_set_style_bg_color(o, lv_color_hex(0x232833), LV_STATE_FOCUSED);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_width(o, ui_px(3), LV_STATE_FOCUSED);
	lv_obj_set_style_outline_pad(o, -ui_px(3), LV_STATE_FOCUSED);
	lv_obj_set_style_outline_color(o, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
	lv_obj_set_style_outline_opa(o, LV_OPA_COVER, LV_STATE_FOCUSED);
}

lv_obj_t *console_row(lv_obj_t *parent)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(o, 2 * ui_cell(), 0);
	lv_obj_set_style_pad_hor(o, ui_cell() / 2, 0);
	lv_obj_set_style_pad_ver(o, ui_px(8), 0);
	lv_obj_set_style_pad_column(o, ui_px(8), 0);
	lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_width(o, 1, 0);
	lv_obj_set_style_border_color(o, UI_COLOR_LINE, 0);
	lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(o, false);
	console_focus_box(o);
	return o;
}

const char *ui_button_name(enum gp_button b)
{
	switch (b) {
	case GP_UP:       return LV_SYMBOL_UP;
	case GP_DOWN:     return LV_SYMBOL_DOWN;
	case GP_LEFT:     return LV_SYMBOL_LEFT;
	case GP_RIGHT:    return LV_SYMBOL_RIGHT;
	case GP_A:        return "A";
	case GP_B:        return "B";
	case GP_X:        return "X";
	case GP_Y:        return "Y";
	case GP_L1:       return "L1";
	case GP_L2:       return "L2";
	case GP_R1:       return "R1";
	case GP_R2:       return "R2";
	case GP_SELECT:   return "Select";
	case GP_START:    return "Start";
	case GP_MENU:     return "Menu";
	case GP_VOL_UP:   return "Vol+";
	case GP_VOL_DOWN: return "Vol-";
	case GP_F1:       return "F1";
	case GP_F2:       return "F2";
	case GP_F3:       return "F3";
	case GP_F4:       return "F4";
	case GP_F5:       return "F5";
	case GP_F6:       return "F6";
	case GP_NONE:     return "\xe2\x80\x94";	/* — */
	default:          return gp_button_name(b);
	}
}

void ui_set_text(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

void screen_console_style(lv_obj_t *screen)
{
	lv_obj_set_style_bg_color(screen, UI_COLOR_BG, 0);
	lv_obj_set_style_bg_grad_color(screen, UI_COLOR_BG2, 0);
	lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_text_color(screen, lv_color_white(), 0);
	lv_obj_set_style_text_font(screen, UI_FONT_SMALL, 0);
	lv_obj_set_scrollable(screen, false);
}

lv_obj_t *console_header(lv_obj_t *screen, const char *symbol, const char *title)
{
	return statusbar_create(screen, symbol, title);
}

static lv_obj_t *hline(lv_obj_t *screen, int32_t y)
{
	lv_obj_t *line = lv_obj_create(screen);

	lv_obj_remove_style_all(line);
	lv_obj_set_size(line, screen_w() - 2 * ui_margin(), 1);
	lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(line, UI_COLOR_LINE, 0);
	lv_obj_set_pos(line, ui_margin(), y);
	return line;
}

lv_obj_t *console_screen(const char *symbol, const char *title)
{
	lv_obj_t *scr = lv_obj_create(NULL);

	screen_console_style(scr);
	console_header(scr, symbol, title);
	hline(scr, ui_header_h());
	return scr;
}

lv_obj_t *screen_body(lv_obj_t *screen)
{
	lv_obj_t *body = lv_obj_create(screen);
	lv_area_t a;

	ui_body_area(&a);
	lv_obj_remove_style_all(body);
	lv_obj_set_pos(body, a.x1, a.y1);
	lv_obj_set_size(body, lv_area_get_width(&a), lv_area_get_height(&a));
	lv_obj_set_style_pad_row(body, ui_cell() / 2, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
	return body;
}

lv_obj_t *footer_create(lv_obj_t *screen)
{
	int32_t fh = footer_height();
	lv_obj_t *box;

	hline(screen, screen_h() - fh);
	box = lv_obj_create(screen);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, LV_SIZE_CONTENT, fh);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(box, ui_px(8), 0);
	lv_obj_align(box, LV_ALIGN_BOTTOM_RIGHT, -ui_margin(), 0);
	return box;
}

void footer_set(lv_obj_t *footer, ...)
{
	int32_t badge_h = ui_px(22);
	const char *button;
	va_list ap;
	bool first = true;

	lv_obj_clean(footer);
	va_start(ap, footer);
	while ((button = va_arg(ap, const char *))) {
		const char *text = va_arg(ap, const char *);
		lv_obj_t *badge = lv_label_create(footer), *label;

		lv_obj_set_height(badge, badge_h);
		lv_obj_set_style_min_width(badge, badge_h, 0);
		/* arrows, "A×2": a pill */
		lv_obj_set_style_pad_hor(badge, button[1] ? ui_px(6) : 0, 0);
		lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
		lv_obj_set_style_text_color(badge, UI_COLOR_BG, 0);
		lv_obj_set_style_text_font(badge, UI_FONT_SMALL, 0);
		lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
		/* the line in the middle of the badge */
		lv_obj_set_style_pad_top(badge, (badge_h - lv_font_get_line_height(UI_FONT_SMALL)) / 2, 0);
		if (!first)
			lv_obj_set_style_margin_left(badge, ui_px(12), 0);
		lv_label_set_text(badge, button);

		label = lv_label_create(footer);
		lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
		lv_label_set_text(label, text);
		first = false;
	}
	va_end(ap);
}
