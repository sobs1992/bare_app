/*
 * Widget skeleton for the LVGL backend. Screens are widgets too.
 *
 *   init    create the LVGL objects under parent (NULL: a new screen),
 *           return private state, put the top object in *root
 *   update  show what changed in the sections listed in 'deps'; called
 *           once right after init with changed = CTX_ALL. Compare with
 *           what is shown before calling lv_*_set_*: every set redraws.
 *   deinit  free private state; LVGL deletes the objects itself
 *
 * The widget lives as long as its root object: deleting the root
 * unsubscribes it and calls deinit.
 */
#pragma once

#include "lvgl_view.h"

struct widget_ops {
	const char *name;
	uint32_t deps;		/* enum ctx_section mask, 0: no context */
	void *(*init)(lv_obj_t *parent, lv_obj_t **root);
	void (*update)(void *self, const struct gui_ctx *ctx, uint32_t changed);
	void (*deinit)(void *self);
};

lv_obj_t *widget_create(const struct widget_ops *ops, lv_obj_t *parent);
void *widget_self(lv_obj_t *root);

/* widgets/ */
extern const struct widget_ops statusbar_widget;
/* System monitor on the top layer, shown while CTX_SYSMON says so */
extern const struct widget_ops sysmon_widget;
/* symbol: LV_SYMBOL_* or NULL */
void statusbar_set_title(lv_obj_t *bar, const char *symbol, const char *title);
lv_obj_t *statusbar_create(lv_obj_t *parent, const char *symbol, const char *title);

/* screens/ */
extern const struct widget_ops home_screen, settings_screen, input_test_screen, layers_screen,
	game_screen, wifi_screen, wifi_key_screen, usb_disk_screen, controls_screen;
/* A game screen is up: it says a low battery itself (screens/game.c) */
bool game_screen_battery(void);

/* widgets/console.c: what every screen shares (the grid: lvgl_view.h) */
/* A new screen: the background, the header with symbol and title, a line under it */
lv_obj_t *console_screen(const char *symbol, const char *title);
/* The header alone (the status bar: its ends at the margins) */
lv_obj_t *console_header(lv_obj_t *screen, const char *symbol, const char *title);
/* A column in ui_body_area() */
lv_obj_t *screen_body(lv_obj_t *screen);
/* A line along the bottom with the buttons' hints at its right, "(B) Back  (A) OK" */
lv_obj_t *footer_create(lv_obj_t *screen);
/* Focused: a lighter box with the accent frame inside it (rows next to each
 * other need no room for it) */
void console_focus_box(lv_obj_t *o);
/* A row of a list: two cells high at least, a line under it, focus box */
lv_obj_t *console_row(lv_obj_t *parent);
/* A button as printed on the device: arrows, "A", "L1", "Select", "F1"; GP_NONE a dash */
const char *ui_button_name(enum gp_button b);
/* Button, text pairs up to a NULL: a letter ("A") or symbols (arrows) */
void footer_set(lv_obj_t *footer, ...);
/* The screen's background: a dark vertical gradient */
void screen_console_style(lv_obj_t *screen);
