/* LVGL backend internals. Only files under gui/view/lvgl include LVGL. */
#pragma once

#include "lvgl.h"

#include "gui/ctx.h"
#include "gui/view/core/view.h"

struct hal_display;

#include "gui/view/core/i18n.h"

#ifdef CONFIG_UI_FONT_CYRILLIC
/* Noto Sans with Cyrillic (fonts/, tools/gen_font.py); icons fall back to Montserrat */
extern const lv_font_t ui_font_14, ui_font_20;
#define UI_FONT_SMALL (&ui_font_14)
#define UI_FONT_MID   (&ui_font_20)
#else
#define UI_FONT_SMALL (&lv_font_montserrat_14)
#define UI_FONT_MID   (&lv_font_montserrat_20)
#endif
#define UI_COLOR_DIM  lv_color_hex(0x8a8f98)
#define UI_COLOR_ON   lv_color_hex(0x2f8cff)
#define UI_COLOR_WARN lv_color_hex(0xff6a3d)
#define UI_STATUSBAR_H 36
/* X as an LVGL key (LV_EVENT_KEY on the focused object); beyond Unicode, so
 * nothing types it */
#define UI_LV_KEY_ALT 0x110000u

/* port_display.c: LVGL display drawing into the HAL UI layer */
lv_display_t *port_display_init(struct hal_display *d);

/* port_input.c: UI keys -> LVGL keypad */
int port_input_init(void);
void port_input_deinit(void);
bool port_input_key(enum ui_key key, bool pressed, bool repeat);
void port_input_set_group(lv_group_t *g);

/* screens.c: shows the screen named by CTX_NAV */
int screens_init(void);
void screens_deinit(void);

/* backend.c */
void ui_toast(const char *text);
/* Volume level over everything; repeated calls update the same box */
void ui_volume_osd(int percent);
/* How far a focus frame reaches out of its object: keep that much free */
struct ui_frame {
	int32_t hor, ver;
};
/* Clear keypad focus frame; the default theme's outline is hard to see.
 * Around a slider it leaves room for the knob. */
struct ui_frame ui_focus_style(lv_obj_t *obj);
