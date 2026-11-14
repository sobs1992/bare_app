/*
 * The battery as a pill filled to the level with the percent inside, shared
 * by the status bar and the game's corner (screens/game.c). The empty part
 * has the fill's colour too, darker, so that the black digits read across
 * the edge of the fill and a digit is never cut in two. White; 20 % and less
 * yellow; charging green with a bolt before the digits. 10 % and less (not
 * charging) is said differently: a dark pill, a faint red fill, red digits.
 *
 * The objects: the row (no style of its own) holds the body and the
 * terminal; the body holds the fill and the digits over it.
 */
#include <stdio.h>

#include "../widget.h"

#define BAT_W 42	/* the pill, in the 480x320 layout */
#define BAT_H 18
#define BAT_EMPTY lv_color_hex(0x2a2e36)	/* the red level's pill */
#define BAT_NUB   lv_color_hex(0x5a5f69)	/* the red level's terminal */
#define BAT_INK   lv_color_hex(0x101216)	/* the digits on a lit pill */
#define BAT_TINT  LV_OPA_50	/* how much of the level's colour the empty part has */
#define BAT_RED_OPA LV_OPA_40	/* the red level's fill, see-through */

static lv_color_t level_color(int cap, bool charging)
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

lv_obj_t *battery_pill_create(lv_obj_t *parent)
{
	lv_obj_t *row, *body, *fill, *text, *nub;

	row = plain_obj(parent);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, LV_MAX(ui_px(1), 1), 0);

	body = plain_obj(row);
	lv_obj_set_size(body, ui_px(BAT_W), ui_px(BAT_H));
	lv_obj_set_style_radius(body, ui_px(5), 0);
	lv_obj_set_style_clip_corner(body, true, 0);
	lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(body, BAT_EMPTY, 0);

	fill = plain_obj(body);
	lv_obj_set_size(fill, 0, LV_PCT(100));
	lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);

	text = lv_label_create(body);	/* after the fill: over it */
	lv_obj_set_style_text_font(text, UI_FONT_SMALL, 0);
	lv_obj_center(text);
	lv_label_set_text(text, "");

	nub = plain_obj(row);
	lv_obj_set_size(nub, LV_MAX(ui_px(2), 2), ui_px(7));
	lv_obj_set_style_radius(nub, 1, 0);
	lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(nub, BAT_NUB, 0);
	return row;
}

void battery_pill_set(lv_obj_t *pill, int capacity, bool charging)
{
	lv_obj_t *body = lv_obj_get_child(pill, 0), *nub = lv_obj_get_child(pill, 1);
	lv_obj_t *fill = lv_obj_get_child(body, 0), *text = lv_obj_get_child(body, 1);
	int cap = LV_CLAMP(0, capacity, 100);
	lv_color_t col = level_color(cap, charging), dim;
	char digits[16];

	snprintf(digits, sizeof(digits), "%s%d", charging ? LV_SYMBOL_CHARGE " " : "", cap);
	ui_set_text(text, digits);
	lv_obj_set_width(fill, ui_px(BAT_W) * cap / 100);
	if (!charging && cap <= 10) {
		lv_obj_set_style_bg_color(body, BAT_EMPTY, 0);
		lv_obj_set_style_bg_color(fill, lv_color_mix(col, BAT_EMPTY, BAT_RED_OPA), 0);
		lv_obj_set_style_text_color(text, col, 0);
		lv_obj_set_style_bg_color(nub, BAT_NUB, 0);
		return;
	}
	dim = lv_color_mix(col, BAT_EMPTY, BAT_TINT);
	lv_obj_set_style_bg_color(body, dim, 0);
	lv_obj_set_style_bg_color(fill, col, 0);
	lv_obj_set_style_text_color(text, BAT_INK, 0);
	/* the terminal is the pill's last bit: filled only when full */
	lv_obj_set_style_bg_color(nub, cap >= 100 ? col : dim, 0);
}
