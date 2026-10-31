/* Small control modules: application lifetime, navigation, system info. */
#include <stdio.h>
#include <string.h>

#include "control.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "gui/ctx.h"
#include "hal/platform.h"
#include "services/config.h"

/* ---- app: quitting ---------------------------------------------------- */

static struct mainloop *app_loop;

static int app_init(const struct control_env *env)
{
	app_loop = env->ml;
	return 0;
}

static void app_action(const struct gui_action *a)
{
	if (a->type == ACT_QUIT) {
		LOGI("app", "quit requested");
		mainloop_quit(app_loop);
	}
}

const struct control_ops ctl_app = {
	.name = "app",
	.init = app_init,
	.on_action = app_action,
};

/* ---- nav: screen stack ------------------------------------------------ */

static int nav_init(const struct control_env *env)
{
	struct gui_ctx *c = ctx_lock();

	(void)env;
	c->nav.stack[0] = SCREEN_HOME;
	c->nav.depth = 1;
	ctx_unlock(CTX_NAV);
	return 0;
}

static void nav_action(const struct gui_action *a)
{
	struct gui_ctx *c;
	uint32_t changed = 0;

	if (a->type != ACT_OPEN_SCREEN && a->type != ACT_BACK)
		return;
	c = ctx_lock();
	if (a->type == ACT_OPEN_SCREEN) {
		if (a->arg >= 0 && a->arg < SCREEN_COUNT && c->nav.depth < NAV_DEPTH &&
		    c->nav.stack[c->nav.depth - 1] != (enum screen_id)a->arg) {
			c->nav.stack[c->nav.depth++] = a->arg;
			changed = CTX_NAV;
		}
	} else if (c->nav.depth > 1) {
		c->nav.depth--;
		changed = CTX_NAV;
	}
	if (changed)
		LOGD("nav", "screen %d, depth %d", c->nav.stack[c->nav.depth - 1], c->nav.depth);
	ctx_unlock(changed);
}

const struct control_ops ctl_nav = {
	.name = "nav",
	.init = nav_init,
	.on_action = nav_action,
};

/* ---- system: what we run on ------------------------------------------- */

static int system_init(const struct control_env *env)
{
	struct gui_ctx *c = ctx_lock();

	snprintf(c->system.platform, sizeof(c->system.platform), "%s", env->pf->name);
	/* "" when unknown: wording is the view's business */
	snprintf(c->system.model, sizeof(c->system.model), "%s", platform_model());
	ctx_unlock(CTX_SYSTEM);
	return 0;
}

const struct control_ops ctl_system = {
	.name = "system",
	.init = system_init,
};

/* ---- settings: UI language (kept in the config file) ------------------ */

static void set_lang(const char *lang)
{
	struct gui_ctx *c = ctx_lock();

	snprintf(c->settings.lang, sizeof(c->settings.lang), "%s", lang);
	ctx_unlock(CTX_SETTINGS);
}

static int settings_init(const struct control_env *env)
{
	const char *lang = env->lang && *env->lang ? env->lang : config_get("lang");

	set_lang(lang ? lang : "en");
	return 0;
}

static void settings_action(const struct gui_action *a)
{
	if (a->type != ACT_SET_LANG || !a->str[0])
		return;
	set_lang(a->str);
	config_set("lang", a->str);
}

const struct control_ops ctl_settings = {
	.name = "settings",
	.init = settings_init,
	.on_action = settings_action,
};
