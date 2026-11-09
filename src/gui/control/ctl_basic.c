/* Small control modules: application lifetime, navigation, system info. */
#include <stdio.h>
#include <string.h>

#include "control.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "gui/ctx.h"
#include "hal/platform.h"
#include "services/config.h"

/* ---- app: quitting, powering off -------------------------------------- */

static struct mainloop *app_loop;
static const char *app_poweroff_avail, *app_poweroff;

static int app_init(const struct control_env *env)
{
	app_loop = env->ml;
	app_poweroff_avail = env->pf->poweroff;
	return 0;
}

static void app_action(const struct gui_action *a)
{
	if (a->type == ACT_POWER_OFF && app_poweroff_avail) {
		LOGI("app", "power off requested");
		app_poweroff = app_poweroff_avail;
		mainloop_quit(app_loop);
	} else if (a->type == ACT_QUIT || a->type == ACT_POWER_OFF) {
		LOGI("app", "quit requested");
		mainloop_quit(app_loop);
	}
}

/* main() runs it after the screen has gone back to the console */
const char *control_poweroff_cmd(void)
{
	return app_poweroff;
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

/* ---- settings: UI language, the games played (kept in the config file) */

static void set_lang(const char *lang)
{
	struct gui_ctx *c = ctx_lock();

	snprintf(c->settings.lang, sizeof(c->settings.lang), "%.*s",
		 (int)sizeof(c->settings.lang) - 1, lang);
	ctx_unlock(CTX_SETTINGS);
}

/* key first, then the others of list but key, at most RECENT_MAX */
static void recent_put(char *out, size_t size, const char *list, const char *key)
{
	size_t len = (size_t)snprintf(out, size, "%s", key), klen = strlen(key);
	int n = 1;

	while (list && *list && n < RECENT_MAX) {
		size_t l = strcspn(list, ",");

		if (l && (l != klen || strncmp(list, key, l)) && len + 1 + l < size) {
			out[len++] = ',';
			memcpy(out + len, list, l);
			out[len += l] = '\0';
			n++;
		}
		list += l + (list[l] == ',');
	}
}

static void set_recent(const char *list)
{
	struct gui_ctx *c = ctx_lock();

	snprintf(c->settings.recent, sizeof(c->settings.recent), "%s", list);
	ctx_unlock(CTX_SETTINGS);
}

static void set_game_hint(bool on)
{
	struct gui_ctx *c = ctx_lock();

	c->settings.game_hint = on;
	ctx_unlock(CTX_SETTINGS);
}

static void set_stream(bool on)
{
	struct gui_ctx *c = ctx_lock();

	c->settings.stream = on;
	ctx_unlock(CTX_SETTINGS);
}

/* ---- a game's own controls: "controls.<game_key()>" in the config ------ */

#define CONTROLS_KEY "controls."

static bool game_key_ok(const char *key)
{
	size_t n = strlen(key);

	return n > 0 && n < GAME_KEY_LEN && strspn(key, "0123456789abcdef") == n;
}

/* The entry of game (added if add), NULL: none or no room */
static struct game_controls *controls_of(struct gui_ctx *c, const char *game, bool add)
{
	struct game_controls *gc;
	int i;

	for (i = 0; i < c->settings.ncontrols; i++)
		if (!strcmp(c->settings.controls[i].game, game))
			return &c->settings.controls[i];
	if (!add || c->settings.ncontrols == CONTROLS_MAX)
		return NULL;
	gc = &c->settings.controls[c->settings.ncontrols++];
	snprintf(gc->game, sizeof(gc->game), "%s", game);
	gp_remap_reset(&gc->map);
	return gc;
}

static void load_controls(const char *key, const char *value, void *ud)
{
	struct gui_ctx *c = ud;
	const char *game = key + strlen(CONTROLS_KEY);
	struct gp_remap m;
	struct game_controls *gc;

	if (!game_key_ok(game))
		return;
	gp_remap_parse(&m, value);
	if (gp_remap_is_default(&m) || !(gc = controls_of(c, game, true)))
		return;
	gc->map = m;
}

/* One button of a game, or all back to the default (from < 0); kept in the config */
static void set_control(const char *game, int from, int to)
{
	struct gui_ctx *c;
	struct game_controls *gc;
	char key[sizeof(CONTROLS_KEY) + GAME_KEY_LEN], text[256];
	bool keep;

	if (!game_key_ok(game) || from >= GP_COUNT || to < GP_NONE || to >= GP_COUNT)
		return;
	c = ctx_lock();
	gc = controls_of(c, game, from >= 0);
	if (!gc) {
		ctx_unlock(0);
		if (from >= 0)
			LOGW("settings", "no room for the controls of another game");
		return;
	}
	if (from >= 0)
		gc->map.to[from] = (int8_t)to;
	else
		gp_remap_reset(&gc->map);
	keep = !gp_remap_is_default(&gc->map);
	gp_remap_format(&gc->map, text, sizeof(text));
	if (!keep)	/* the default has no entry */
		*gc = c->settings.controls[--c->settings.ncontrols];
	ctx_unlock(CTX_SETTINGS);
	snprintf(key, sizeof(key), CONTROLS_KEY "%s", game);
	if (keep)
		config_set(key, text);
	else
		config_unset(key);
}

static int settings_init(const struct control_env *env)
{
	const char *lang = env->lang && *env->lang ? env->lang : config_get("lang");
	const char *recent = config_get("recent");

	set_lang(lang ? lang : "en");
	set_recent(recent ? recent : "");
	set_game_hint(config_get_int("game_hint", 0));
	set_stream(config_get_int("stream", 0));
	config_each(CONTROLS_KEY, load_controls, ctx_lock());
	ctx_unlock(CTX_SETTINGS);
	return 0;
}

static void settings_action(const struct gui_action *a)
{
	char list[sizeof(((struct gui_ctx *)0)->settings.recent)];

	if (a->type == ACT_SET_GAME_HINT) {
		set_game_hint(a->arg);
		config_set_int("game_hint", !!a->arg);
		return;
	}
	if (a->type == ACT_SET_STREAM) {
		set_stream(a->arg);
		config_set_int("stream", !!a->arg);
		return;
	}
	if (a->type == ACT_SET_CONTROL) {
		set_control(a->str, ACT_CONTROL_FROM(a->arg), ACT_CONTROL_TO(a->arg));
		return;
	}
	if (a->type == ACT_RESET_CONTROLS) {
		set_control(a->str, -1, 0);
		return;
	}
	if (!a->str[0] || strchr(a->str, ','))
		return;
	if (a->type == ACT_SET_LANG) {
		set_lang(a->str);
		config_set("lang", a->str);
	} else if (a->type == ACT_GAME_PLAYED) {
		recent_put(list, sizeof(list), config_get("recent"), a->str);
		set_recent(list);
		config_set("recent", list);
	}
}

const struct control_ops ctl_settings = {
	.name = "settings",
	.init = settings_init,
	.on_action = settings_action,
};
