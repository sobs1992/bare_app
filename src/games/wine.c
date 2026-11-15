/*
 * Windows games under Wine (GAME_WINE): every <games>/<dir>/wine.ini is an
 * entry of the main menu. A game is played by bare_app_wine (ports/wine),
 * which runs Wine with the SDK's winebare.drv driver, turns its windows
 * into frames of the video layer and the gamepad into mouse and keyboard
 * (the [keys] of wine.ini). Nothing of Wine is linked into bare_app.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "game.h"
#include "ini.h"

#define TAG "wine"
#define HOST "bare_app_wine"
#define WINE_MAX_GAMES 16

struct wine_game {
	char dir[PATH_MAX];
	char name[64];
	char exe[256];
	char hint[512], hint_ru[512];	/* the controls, "" the generic hint */
	char cover[PATH_MAX];		/* "cover" of wine.ini, relative to dir */
	int minimize;			/* "minimize": f1..f6, -1 F5 (games' own) */
	bool trigger_volume;		/* "volume = triggers": L2, R2 */
	/* [keys] as written: a control's action or a stick's use, "" not
	 * given; the D-pad's use, the right stick's direction keys */
	char keys[GAME_CONTROLS][48];
	char dpad[12], rstick_dir[4][32];
	char labels[2][GAME_CONTROLS][64];	/* [labels], [labels.ru] */
};

static struct wine_game found[WINE_MAX_GAMES];
static int count;

/* [keys] and [labels] of wine.ini */
static void section_value(struct wine_game *g, const char *section, const char *key, const char *value)
{
	static const char *const dirs[4] = { "rstick_up", "rstick_down", "rstick_left", "rstick_right" };
	int c = game_control_by_name(key), i;

	if (!strcasecmp(section, "keys")) {
		if (c >= 0)
			snprintf(g->keys[c], sizeof(g->keys[c]), "%s", value);
		else if (!strcasecmp(key, "dpad"))
			snprintf(g->dpad, sizeof(g->dpad), "%s", value);
		for (i = 0; i < 4; i++)
			if (!strcasecmp(key, dirs[i]))
				snprintf(g->rstick_dir[i], sizeof(g->rstick_dir[i]), "%s", value);
	} else if (c >= 0 && (!strcasecmp(section, "labels") || !strcasecmp(section, "labels.ru"))) {
		i = !strcasecmp(section, "labels.ru");
		snprintf(g->labels[i][c], sizeof(g->labels[i][c]), "%s", value);
	}
}

/* name, exe and hint of wine.ini before its first [section], then the
 * [keys] and [labels] */
static bool ini_value(const char *section, const char *key, const char *value, void *ud)
{
	struct wine_game *g = ud;

	if (*section) {
		section_value(g, section, key, value);
		return true;
	}
	if (!strcasecmp(key, "name"))
		snprintf(g->name, sizeof(g->name), "%s", value);
	else if (!strcasecmp(key, "exe"))
		snprintf(g->exe, sizeof(g->exe), "%s", value);
	else if (!strcasecmp(key, "hint"))
		ini_copy_lines(g->hint, sizeof(g->hint), value);
	else if (!strcasecmp(key, "hint.ru"))
		ini_copy_lines(g->hint_ru, sizeof(g->hint_ru), value);
	else if (!strcasecmp(key, "cover"))
		snprintf(g->cover, sizeof(g->cover), "%s", value);
	else if (!strcasecmp(key, "minimize")) {
		static const enum gp_button f[] = { GP_F1, GP_F2, GP_F3, GP_F4, GP_F5, GP_F6 };

		if ((value[0] == 'f' || value[0] == 'F') && value[1] >= '1' && value[1] <= '6' && !value[2])
			g->minimize = f[value[1] - '1'];
		else
			LOGW(TAG, "%s: minimize = %s?", g->dir, value);
	} else if (!strcasecmp(key, "volume"))
		g->trigger_volume = !strcasecmp(value, "triggers");
	return true;
}

/* false if there is no wine.ini */
static bool read_ini(struct wine_game *g)
{
	char path[PATH_MAX + 16];

	snprintf(path, sizeof(path), "%s/wine.ini", g->dir);
	return ini_read(path, ini_value, g) == 0;
}

static int by_name(const void *a, const void *b)
{
	return strcasecmp(((const struct wine_game *)a)->name, ((const struct wine_game *)b)->name);
}

/* Looks for the games again, in the order of their names */
static int wine_scan(void)
{
	const char *root = game_setting("BARE_APP_WINE_GAMES", CONFIG_GAME_WINE_GAMES);
	struct dirent *e;
	DIR *d = opendir(root);

	count = 0;
	while (d && (e = readdir(d)) && count < WINE_MAX_GAMES) {
		struct wine_game *g = &found[count];

		if (e->d_name[0] == '.')
			continue;
		if (snprintf(g->dir, sizeof(g->dir), "%s/%s", root, e->d_name) >= (int)sizeof(g->dir))
			continue;
		g->name[0] = g->exe[0] = g->hint[0] = g->hint_ru[0] = g->cover[0] = '\0';
		memset(g->keys, 0, sizeof(g->keys));
		memset(g->labels, 0, sizeof(g->labels));
		memset(g->rstick_dir, 0, sizeof(g->rstick_dir));
		g->dpad[0] = '\0';
		g->minimize = -1;
		g->trigger_volume = false;
		if (!read_ini(g))
			continue;
		if (g->cover[0] != '/') {	/* relative to the game, cover.png by default */
			char cover[PATH_MAX];

			if (snprintf(cover, sizeof(cover), "%s/%s", g->dir,
				     g->cover[0] ? g->cover : "cover.png") < (int)sizeof(cover))
				snprintf(g->cover, sizeof(g->cover), "%s", cover);
		}
		if (!g->name[0])
			snprintf(g->name, sizeof(g->name), "%.63s", e->d_name);
		count++;
	}
	if (d)
		closedir(d);
	qsort(found, count, sizeof(found[0]), by_name);
	return count;
}

static int wine_entries(bool rescan)
{
	return rescan ? wine_scan() : count;
}

/* The "name" of wine.ini, else the directory's name */
static void wine_info(int i, struct game_info *gi)
{
	const struct wine_game *g = i >= 0 && i < count ? &found[i] : NULL;

	gi->name = g ? g->name : "";
	if (g) {
		gi->hint = !strncmp(i18n_lang(), "ru", 2) && g->hint_ru[0] ? g->hint_ru
			   : g->hint[0] ? g->hint : NULL;
		gi->cover = g->cover;
		gi->minimize = g->minimize;
		gi->trigger_volume = g->trigger_volume;
	}
}

/*
 * What a control does, for the controls screen: [labels] (or [labels.ru]
 * in Russian) of wine.ini, else what [keys] makes of it ("Enter+Q", "left
 * click", "mouse"), with bare_app_wine's set_defaults() for what [keys]
 * leaves out.
 */
static const char *const default_keys[GAME_CONTROLS] = {
	[GP_UP] = "up", [GP_DOWN] = "down", [GP_LEFT] = "left", [GP_RIGHT] = "right",
	[GP_A] = "lbutton", [GP_B] = "rbutton", [GP_X] = "enter", [GP_Y] = "space",
	[GP_L1] = "pgup", [GP_R1] = "pgdn", [GP_L2] = "shift", [GP_R2] = "ctrl",
	[GP_SELECT] = "tab", [GP_START] = "esc",
	[GP_F1] = "f1", [GP_F2] = "f2", [GP_F3] = "f3", [GP_F4] = "f4",
	[GAME_STICK_LEFT] = "mouse", [GAME_STICK_RIGHT] = "wheel",
};

/* One key or mouse action of [keys] as shown: "Esc", "Num 5", "left click" */
static void key_text(char *out, size_t size, const char *k)
{
	static const struct { const char *key; enum str_id id; } words[] = {
		{ "lbutton", S_CTL_LCLICK }, { "rbutton", S_CTL_RCLICK }, { "mbutton", S_CTL_MCLICK },
		{ "wheelup", S_CTL_WHEEL_UP }, { "wheeldown", S_CTL_WHEEL_DOWN },
		{ "magnify", S_CTL_MAGNIFY }, { "space", S_CTL_SPACE }, { "up", S_CTL_UP },
		{ "down", S_CTL_DOWN }, { "left", S_CTL_LEFT }, { "right", S_CTL_RIGHT },
	};
	static const struct { const char *key, *text; } names[] = {
		{ "esc", "Esc" }, { "enter", "Enter" }, { "tab", "Tab" }, { "backspace", "Backspace" },
		{ "shift", "Shift" }, { "lshift", "Shift" }, { "rshift", "Shift" },
		{ "ctrl", "Ctrl" }, { "lctrl", "Ctrl" }, { "rctrl", "Ctrl" },
		{ "alt", "Alt" }, { "lalt", "Alt" }, { "ralt", "Alt" },
		{ "pgup", "PgUp" }, { "pgdn", "PgDn" }, { "home", "Home" }, { "end", "End" },
		{ "insert", "Ins" }, { "delete", "Del" }, { "pause", "Pause" }, { "capslock", "CapsLock" },
	};
	size_t i;

	for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		if (!strcasecmp(k, words[i].key)) {
			snprintf(out, size, "%s", tr(words[i].id));
			return;
		}
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcasecmp(k, names[i].key)) {
			snprintf(out, size, "%s", names[i].text);
			return;
		}
	if (!strncasecmp(k, "kp", 2) && k[2]) {
		snprintf(out, size, "Num %s", k + 2);
		return;
	}
	/* f1..f24, letters, digits, signs: as on the key */
	for (i = 0; k[i] && i + 1 < size; i++)
		out[i] = (char)toupper((unsigned char)k[i]);
	if (size)
		out[i] = '\0';
}

/* An action of [keys] ("enter+q", "lbutton", "none") as shown */
static void action_text(char *out, size_t size, const char *v)
{
	char buf[48], part[32], *p, *save = NULL;
	size_t n = 0;

	out[0] = '\0';
	if (!strcasecmp(v, "none"))
		return;
	snprintf(buf, sizeof(buf), "%s", v);
	for (p = strtok_r(buf, "+", &save); p && n + 1 < size; p = strtok_r(NULL, "+", &save)) {
		key_text(part, sizeof(part), p);
		n += (size_t)snprintf(out + n, size - n, "%s%s", n ? "+" : "", part);
	}
}

/* What a stick is used as ([keys] stick, rstick) */
static void stick_text(const struct wine_game *g, int c, char *out, size_t size)
{
	const char *use = g->keys[c][0] ? g->keys[c] : default_keys[c];
	static const struct { const char *use; enum str_id id; } uses[] = {
		{ "mouse", S_CTL_MOUSE }, { "arrows", S_CTL_ARROWS }, { "wheel", S_CTL_WHEEL },
		{ "numpad", S_CTL_NUMPAD }, { "turn", S_CTL_TURN }, { "scroll", S_CTL_SCROLL },
	};
	size_t i, n = 0;

	out[0] = '\0';
	/* the right stick's own keys: "Home/End/Del/PgDn" */
	if (c == GAME_STICK_RIGHT && !strcasecmp(use, "arrows") &&
	    (g->rstick_dir[0][0] || g->rstick_dir[1][0] || g->rstick_dir[2][0] || g->rstick_dir[3][0])) {
		for (i = 0; i < 4 && n + 1 < size; i++) {
			char part[48];

			action_text(part, sizeof(part), g->rstick_dir[i][0] ? g->rstick_dir[i]
							   : default_keys[GP_UP + i]);
			n += (size_t)snprintf(out + n, size - n, "%s%s", i ? "/" : "", part);
		}
		return;
	}
	for (i = 0; i < sizeof(uses) / sizeof(uses[0]); i++)
		if (!strcasecmp(use, uses[i].use)) {
			snprintf(out, size, "%s", tr(uses[i].id));
			return;
		}
}

static const char *wine_label(int i, int c)
{
	static char shown[GAME_CONTROLS][96];
	const struct wine_game *g;
	int ru = !strncmp(i18n_lang(), "ru", 2);

	if (i < 0 || i >= count || c < 0 || c >= GAME_CONTROLS)
		return NULL;
	g = &found[i];
	if (g->labels[ru][c][0])
		return g->labels[ru][c];
	if (c >= GAME_STICK_LEFT)
		stick_text(g, c, shown[c], sizeof(shown[c]));
	else if (c <= GP_RIGHT && !strcasecmp(g->dpad, "mouse"))
		snprintf(shown[c], sizeof(shown[c]), "%s", tr(S_CTL_CURSOR));
	else if (c <= GP_RIGHT && !strcasecmp(g->dpad, "scroll"))
		snprintf(shown[c], sizeof(shown[c]), "%s", tr(S_CTL_SCROLL));
	else
		action_text(shown[c], sizeof(shown[c]),
			    g->keys[c][0] ? g->keys[c] : default_keys[c] ? default_keys[c] : "none");
	return shown[c];
}

static const char *wine_missing(int i)
{
	static char path[PATH_MAX * 2];
	const struct wine_game *g;

	if (i < 0 || i >= count)
		return game_setting("BARE_APP_WINE_GAMES", CONFIG_GAME_WINE_GAMES);
	g = &found[i];
	if (game_proc_sibling(HOST, path, sizeof(path)) < 0 || access(path, X_OK) < 0)
		return path;
	snprintf(path, sizeof(path), "%s/wine", game_setting("BARE_APP_WINE_BIN", CONFIG_GAME_WINE_BIN));
	if (access(path, X_OK) < 0)
		return path;
	/* a path of its own (C:\...) is not checked */
	if (!g->exe[0] || g->exe[0] == '/' || g->exe[1] != ':') {
		if (g->exe[0] == '/')
			snprintf(path, sizeof(path), "%s", g->exe);
		else
			snprintf(path, sizeof(path), "%s/%s", g->dir, g->exe[0] ? g->exe : "wine.ini: exe");
		if (access(path, R_OK) < 0)
			return path;
	}
	return NULL;
}

static struct game_proc *wine_start(int i, const struct game_env *ge)
{
	char host[PATH_MAX];
	struct game_vars env = { 0 };
	const char *missing = wine_missing(i);
	const struct wine_game *g;

	if (missing) {
		LOGE(TAG, "%s: %s", missing, strerror(ENOENT));
		errno = ENOENT;
		return NULL;
	}
	g = &found[i];
	game_proc_sibling(HOST, host, sizeof(host));
	game_vars_add(&env, "BARE_APP_WINE_PREFIX=%s",
		      game_setting("BARE_APP_WINE_PREFIX", CONFIG_GAME_WINE_PREFIX));
	game_vars_add(&env, "BARE_APP_WINE_BIN=%s", game_setting("BARE_APP_WINE_BIN", CONFIG_GAME_WINE_BIN));

	char *const argv[] = { (char *)HOST, (char *)g->dir, NULL };
	struct game_spawn s = { .path = host, .argv = argv, .env = env.v, .dir = g->dir,
				.width = ge->width, .height = ge->height, .keyboard = true,
				.mouse = true };

	LOGI(TAG, "%s: %s", g->name, g->dir);
	return game_proc_start(&s);
}

/* L2 and R2 are the game's (wine.ini [keys]) unless "volume = triggers"; F5
 * minimises unless "minimize" names another F key */
const struct game_def game_wine = {
	.id = "wine",
	.hint = S_WINE_HINT,
	.entries = wine_entries,
	.info = wine_info,
	.missing = wine_missing,
	.start = wine_start,
	.label = wine_label,
};
