/*
 * Windows games under Wine (GAME_WINE): every <games>/<dir>/wine.ini is an
 * entry of the main menu. A game is played by bare_app_wine (ports/wine),
 * which runs Wine with the SDK's winebare.drv driver, turns its windows
 * into frames of the video layer and the gamepad into mouse and keyboard
 * (the [keys] of wine.ini). Nothing of Wine is linked into bare_app.
 */
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
};

static struct wine_game found[WINE_MAX_GAMES];
static int count;

/* name and exe of wine.ini, before its first [section] */
static bool ini_value(const char *section, const char *key, const char *value, void *ud)
{
	struct wine_game *g = ud;

	if (*section)
		return false;
	if (!strcasecmp(key, "name"))
		snprintf(g->name, sizeof(g->name), "%s", value);
	else if (!strcasecmp(key, "exe"))
		snprintf(g->exe, sizeof(g->exe), "%s", value);
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
		g->name[0] = g->exe[0] = '\0';
		if (!read_ini(g))
			continue;
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
	gi->name = i >= 0 && i < count ? found[i].name : "";
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
				.width = ge->width, .height = ge->height };

	LOGI(TAG, "%s: %s", g->name, g->dir);
	return game_proc_start(&s);
}

/* L2 and R2 are the game's (wine.ini [keys]) */
const struct game_def game_wine = {
	.id = "wine",
	.hint = S_WINE_HINT,
	.entries = wine_entries,
	.info = wine_info,
	.missing = wine_missing,
	.start = wine_start,
};
