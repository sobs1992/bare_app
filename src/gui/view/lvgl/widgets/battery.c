/*
 * The battery as a frame with the percent inside, shared by the status bar
 * and the game's corner (screens/game.c): an outline and the terminal in the
 * level's colour, a thin gap, and the cell filled to the level. The empty
 * part of the cell has the fill's colour too, darker, so that the black
 * digits read across the edge of the fill and a digit is never cut in two.
 * White; 20 % and less yellow; charging green with a bolt before the digits.
 * 10 % and less (not charging) is said differently: a red frame round a dark
 * cell, a faint red fill, red digits.
 *
 * The objects: the row (no style of its own) holds the frame and the
 * terminal; the frame holds the cell; the cell holds the fill and the
 * digits over it.
 */
#include <stdio.h>

#include "../widget.h"

#define BAT_W 46	/* the frame, in the 480x320 layout */
#define BAT_H 22
#define BAT_LINE 2	/* the outline */
#define BAT_EMPTY lv_color_hex(0x2a2e36)	/* the red level's cell */
#define BAT_INK   lv_color_hex(0x101216)	/* the digits on a lit cell */
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

lv_obj_t *battery_icon_create(lv_obj_t *parent)
{
	int32_t one = LV_MAX(ui_px(1), 1);
	lv_obj_t *row, *frame, *cell, *fill, *text, *nub;

	row = plain_obj(parent);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, one, 0);

	/* the gap between the outline and the cell is the frame's padding */
	frame = plain_obj(row);
	lv_obj_set_size(frame, ui_px(BAT_W), ui_px(BAT_H));
	lv_obj_set_style_radius(frame, ui_px(6), 0);
	lv_obj_set_style_border_width(frame, LV_MAX(ui_px(BAT_LINE), 2), 0);
	lv_obj_set_style_border_opa(frame, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(frame, one, 0);

	cell = plain_obj(frame);
	lv_obj_set_size(cell, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_radius(cell, ui_px(3), 0);
	lv_obj_set_style_clip_corner(cell, true, 0);
	lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);

	fill = plain_obj(cell);
	lv_obj_set_size(fill, 0, LV_PCT(100));
	lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);

	text = lv_label_create(cell);	/* after the fill: over it */
	lv_obj_set_style_text_font(text, UI_FONT_SMALL, 0);
	lv_obj_center(text);
	lv_label_set_text(text, "");

	nub = plain_obj(row);
	lv_obj_set_size(nub, LV_MAX(ui_px(3), 3), ui_px(8));
	lv_obj_set_style_radius(nub, 1, 0);
	lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
	return row;
}

void battery_icon_set(lv_obj_t *icon, int capacity, bool charging)
{
	lv_obj_t *frame = lv_obj_get_child(icon, 0), *nub = lv_obj_get_child(icon, 1);
	lv_obj_t *cell = lv_obj_get_child(frame, 0);
	lv_obj_t *fill = lv_obj_get_child(cell, 0), *text = lv_obj_get_child(cell, 1);
	int cap = LV_CLAMP(0, capacity, 100);
	lv_color_t col = level_color(cap, charging);
	char digits[16];

	snprintf(digits, sizeof(digits), "%s%d", charging ? LV_SYMBOL_CHARGE " " : "", cap);
	ui_set_text(text, digits);
	lv_obj_set_width(fill, LV_PCT(cap));
	lv_obj_set_style_border_color(frame, col, 0);
	lv_obj_set_style_bg_color(nub, col, 0);
	if (!charging && cap <= 10) {
		lv_obj_set_style_bg_color(cell, BAT_EMPTY, 0);
		lv_obj_set_style_bg_color(fill, lv_color_mix(col, BAT_EMPTY, BAT_RED_OPA), 0);
		lv_obj_set_style_text_color(text, col, 0);
		return;
	}
	lv_obj_set_style_bg_color(cell, lv_color_mix(col, BAT_EMPTY, BAT_TINT), 0);
	lv_obj_set_style_bg_color(fill, col, 0);
	lv_obj_set_style_text_color(text, BAT_INK, 0);
}
