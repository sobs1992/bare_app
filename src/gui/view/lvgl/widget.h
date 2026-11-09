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
	game_screen, wifi_screen, wifi_key_screen, usb_disk_screen;

/* A vertical screen body under the status bar */
lv_obj_t *screen_body(lv_obj_t *screen);

/* widgets/footer.c: the console-like screens (home, settings) */
/* Left and right of everything on them */
int32_t ui_margin(void);
/* The status bar over the background, its ends at the margins, h high */
lv_obj_t *console_header(lv_obj_t *screen, const char *symbol, const char *title, int32_t h);
/* A line along the bottom with the buttons' hints at its right, "(B) Back  (A) OK" */
int32_t footer_height(void);
lv_obj_t *footer_create(lv_obj_t *screen);
/* Button, text pairs up to a NULL: a letter ("A") or symbols (arrows) */
void footer_set(lv_obj_t *footer, ...);
/* The screen's background: a dark vertical gradient */
void screen_console_style(lv_obj_t *screen);
