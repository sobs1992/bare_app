#include "wine.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"

#define TAG "wine"
#define HOST "bare_app_wine"

struct wine_game {
	char dir[PATH_MAX];
	char name[64];
	char exe[256];
};

static struct wine_game games[WINE_MAX_GAMES];
static int count, selected;

static const char *env_or(const char *name, const char *def)
{
	const char *env = getenv(name);

	return env && *env ? env : def;
}

static char *trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

/* name and exe of <dir>/wine.ini (before its first [section]); false if
 * there is no wine.ini */
static bool read_ini(struct wine_game *g)
{
	char path[PATH_MAX + 16], line[512], *eq, *key, *value;
	FILE *f;

	snprintf(path, sizeof(path), "%s/wine.ini", g->dir);
	f = fopen(path, "r");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		key = trim(line);
		if (*key == '[')
			break;
		if (*key == ';' || *key == '#' || !(eq = strchr(key, '=')))
			continue;
		*eq = '\0';
		value = trim(eq + 1);
		/* a comment after the value */
		for (char *c = value; *c; c++)
			if ((*c == ';' || *c == '#') && c > value && isspace((unsigned char)c[-1])) {
				*c = '\0';
				value = trim(value);
				break;
			}
		key = trim(key);
		if (!strcasecmp(key, "name"))
			snprintf(g->name, sizeof(g->name), "%s", value);
		else if (!strcasecmp(key, "exe"))
			snprintf(g->exe, sizeof(g->exe), "%s", value);
	}
	fclose(f);
	return true;
}

static int by_name(const void *a, const void *b)
{
	return strcasecmp(((const struct wine_game *)a)->name, ((const struct wine_game *)b)->name);
}

int wine_scan(void)
{
	const char *root = env_or("BARE_APP_WINE_GAMES", CONFIG_GAME_WINE_GAMES);
	struct dirent *e;
	DIR *d = opendir(root);

	count = 0;
	while (d && (e = readdir(d)) && count < WINE_MAX_GAMES) {
		struct wine_game *g = &games[count];

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
	qsort(games, count, sizeof(games[0]), by_name);
	if (selected >= count)
		selected = 0;
	return count;
}

int wine_count(void)
{
	return count;
}

const char *wine_name(int i)
{
	return i >= 0 && i < count ? games[i].name : "";
}

void wine_select(int i)
{
	if (i >= 0 && i < count)
		selected = i;
}

int wine_selected(void)
{
	return selected;
}

const char *wine_selected_name(void)
{
	return wine_name(selected);
}

const char *wine_missing(void)
{
	static char path[PATH_MAX * 2];
	const struct wine_game *g = &games[selected];

	if (!count)
		return env_or("BARE_APP_WINE_GAMES", CONFIG_GAME_WINE_GAMES);
	if (game_proc_sibling(HOST, path, sizeof(path)) < 0 || access(path, X_OK) < 0)
		return path;
	snprintf(path, sizeof(path), "%s/wine", env_or("BARE_APP_WINE_BIN", CONFIG_GAME_WINE_BIN));
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

struct game_proc *wine_start(int width, int height)
{
	char host[PATH_MAX], prefix[PATH_MAX + 32], bin[PATH_MAX + 32];
	const char *missing = wine_missing();
	const struct wine_game *g = &games[selected];

	if (missing) {
		LOGE(TAG, "%s: %s", missing, strerror(ENOENT));
		errno = ENOENT;
		return NULL;
	}
	game_proc_sibling(HOST, host, sizeof(host));
	snprintf(prefix, sizeof(prefix), "BARE_APP_WINE_PREFIX=%s",
		 env_or("BARE_APP_WINE_PREFIX", CONFIG_GAME_WINE_PREFIX));
	snprintf(bin, sizeof(bin), "BARE_APP_WINE_BIN=%s", env_or("BARE_APP_WINE_BIN", CONFIG_GAME_WINE_BIN));

	char *const argv[] = { (char *)HOST, (char *)g->dir, NULL };
	const char *const env[] = { prefix, bin, NULL };
	struct game_spawn s = { .path = host, .argv = argv, .env = env, .dir = g->dir,
				.width = width, .height = height };

	LOGI(TAG, "%s: %s", g->name, g->dir);
	return game_proc_start(&s);
}
