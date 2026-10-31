/* Display: backlight level into CTX_DISPLAY, ACT_SET_BRIGHTNESS. */
#include "control.h"
#include "core/log.h"
#include "gui/ctx.h"
#include "services/config.h"
#include "services/services.h"

static void publish(void)
{
	struct gui_ctx *c = ctx_lock();

	c->display.has_backlight = backlight_available();
	c->display.brightness = backlight_available() ? backlight_get() : -1;
	ctx_unlock(CTX_DISPLAY);
}

static int display_init(const struct control_env *env)
{
	int saved = config_get_int("brightness", -1);

	(void)env;
	if (saved > 0 && backlight_available())
		backlight_set(saved);
	publish();
	return 0;
}

static void display_action(const struct gui_action *a)
{
	if (a->type != ACT_SET_BRIGHTNESS || !backlight_available())
		return;
	if (backlight_set(a->arg) < 0)
		LOGW("display", "cannot set brightness %d%%", a->arg);
	publish();	/* what the hardware really took */
	config_set_int("brightness", backlight_get());
}

const struct control_ops ctl_display = {
	.name = "display",
	.init = display_init,
	.on_action = display_action,
};
