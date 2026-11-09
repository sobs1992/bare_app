/*
 * Settings kept in the config file (CTX_SETTINGS): the UI language, the
 * games in the order they were played, the controls hint at a game's
 * start, the stream, each game's own controls ("controls.<game_key()>").
 */
#include <stdio.h>
#include <string.h>

#include "control.h"
#include "core/log.h"
#include "gui/ctx.h"
#include "services/config.h"

#define TAG "settings"

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
			LOGW(TAG, "no room for the controls of another game");
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
