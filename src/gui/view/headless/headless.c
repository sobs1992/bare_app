/*
 * Headless backend: draws nothing, logs what a real view would show.
 * Proves that the core builds without any toolkit and lets the control
 * side be exercised in scripts.
 */
#include <stdio.h>

#include "core/log.h"
#include "gui/action.h"
#include "gui/view/core/view.h"

#define TAG "headless"

static int sub;

static void on_ctx(void *self, const struct gui_ctx *c, uint32_t changed)
{
	(void)self;
	if (changed & CTX_NAV)
		LOGI(TAG, "screen %d (depth %d)", ctx_screen(c), c->nav.depth);
	if (changed & CTX_POWER)
		LOGI(TAG, "power: %s, %d%%%s%s", c->power.available ? "battery" : "none",
		     c->power.capacity, c->power.charging ? ", charging" : "",
		     c->power.online ? ", online" : "");
	if (changed & CTX_DISPLAY)
		LOGI(TAG, "backlight: %s %d%%", c->display.has_backlight ? "yes" : "no",
		     c->display.brightness);
	if (changed & CTX_SETTINGS)
		LOGI(TAG, "language: %s", c->settings.lang);
	if (changed & CTX_SYSTEM)
		LOGI(TAG, "system: %s on %s", c->system.platform, c->system.model);
}

static int headless_init(struct hal_display *display, struct mainloop *ml)
{
	(void)display;
	(void)ml;
	sub = view_subscribe(CTX_ALL, on_ctx, NULL);
	return sub > 0 ? 0 : -1;
}

static bool headless_key(enum ui_key key, bool pressed, bool repeat)
{
	if (pressed)
		LOGI(TAG, "key %s%s", ui_key_name(key), repeat ? " (repeat)" : "");
	/* the only navigation headless offers: Accept opens the input test */
	if (key == UI_KEY_ACCEPT && pressed && !repeat &&
	    ctx_screen(view_ctx()) == SCREEN_HOME)
		gui_action_post(ACT_OPEN_SCREEN, SCREEN_INPUT_TEST);
	return false;
}

static void headless_notify(const struct gui_notify *n)
{
	LOGI(TAG, "notify %d (%d)", n->kind, n->value);
}

static void headless_deinit(void)
{
	view_unsubscribe(sub);
}

const struct view_backend_ops view_backend_headless = {
	.name = "headless",
	.init = headless_init,
	.on_key = headless_key,
	.on_notify = headless_notify,
	.process = NULL,
	.deinit = headless_deinit,
};
