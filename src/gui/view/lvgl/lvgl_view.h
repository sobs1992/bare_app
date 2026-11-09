/* LVGL backend internals. Only files under gui/view/lvgl include LVGL. */
#pragma once

#include "lvgl.h"

#include "gui/ctx.h"
#include "gui/view/core/view.h"

struct hal_display;

#include "gui/view/core/i18n.h"

#ifdef CONFIG_UI_FONT_CYRILLIC
/* Noto Sans with Cyrillic (fonts/, tools/gen_font.py); icons fall back to Montserrat */
extern const lv_font_t ui_font_14, ui_font_20, ui_font_28;
#endif
/* The two text sizes, by the screen's size (widgets/console.c) */
enum ui_text {
	UI_TEXT_SMALL, UI_TEXT_MID,
	UI_TEXT_COMPACT,	/* 14 px everywhere: dense panels (the system monitor) */
};
const lv_font_t *ui_font(enum ui_text size);
#define UI_FONT_SMALL ui_font(UI_TEXT_SMALL)
#define UI_FONT_MID   ui_font(UI_TEXT_MID)
#define UI_COLOR_DIM  lv_color_hex(0x8a8f98)
#define UI_COLOR_ON   lv_color_hex(0x2f8cff)
#define UI_COLOR_WARN lv_color_hex(0xff6a3d)
/* The console-like screens (home, settings) */
#define UI_COLOR_BG     lv_color_hex(0x15181e)	/* top of the background */
#define UI_COLOR_BG2    lv_color_hex(0x0d0f13)	/* bottom */
#define UI_COLOR_LINE   lv_color_hex(0x2a2f39)
#define UI_COLOR_ACCENT lv_color_hex(0x31c6ff)	/* focus frames, what is chosen */
/*
 * The grid of every screen (widgets/console.c): UI_ROWS square cells high,
 * the margins at the sides equal; the header and the footer two rows each.
 */
#define UI_ROWS 16
int32_t ui_cell(void);		/* side of a cell, pixels */
int32_t ui_px(int32_t px);	/* a size of the 480x320 layout (cell 20) on this screen */
int32_t ui_margin(void);	/* left and right of everything */
int32_t ui_header_h(void);
int32_t footer_height(void);
void ui_body_area(lv_area_t *a);	/* between them, half a cell off each */
/* X as an LVGL key (LV_EVENT_KEY on the focused object); beyond Unicode, so
 * nothing types it */
#define UI_LV_KEY_ALT 0x110000u

/* lv_label_set_text() redraws the label even when the text is the same:
 * this sets it only when it differs */
void ui_set_text(lv_obj_t *label, const char *text);

/* port_display.c: LVGL display drawing into the HAL UI layer */
lv_display_t *port_display_init(struct hal_display *d);

/* port_input.c: UI keys -> LVGL keypad */
int port_input_init(void);
void port_input_deinit(void);
bool port_input_key(enum ui_key key, bool pressed, bool repeat);
void port_input_set_group(lv_group_t *g);
/* The objects of g move the focus themselves: they get LV_KEY_UP/DOWN as
 * LV_EVENT_KEY instead of the focus going to the previous/next (NULL: none) */
void port_input_set_2d(lv_group_t *g);
lv_group_t *port_input_2d(void);
/* From LV_EVENT_CANCEL (B) of an object: B is used, the screen stays */
void port_input_take_back(void);

/* screens.c: shows the screen named by CTX_NAV */
int screens_init(void);
void screens_deinit(void);

/* backend.c */
void ui_toast(const char *text);
/* Volume level over everything; repeated calls update the same box */
void ui_volume_osd(int percent);
/* LV_SYMBOL_MUTE / VOLUME_MID / VOLUME_MAX for a level */
const char *ui_volume_symbol(int percent);
/* How far a focus frame reaches out of its object: keep that much free */
struct ui_frame {
	int32_t hor, ver;
};
/* Clear keypad focus frame; the default theme's outline is hard to see.
 * Around a slider it leaves room for the knob. */
struct ui_frame ui_focus_style(lv_obj_t *obj);
