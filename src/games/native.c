/*
 * Games described by a game.ini (GAME_NATIVE): <dir>/<game>.ini and
 * <dir>/<game>/game.ini of every directory of GAME_NATIVE_DIRS are entries
 * of the main menu. Each is a program of its own that bare_app starts with
 * its link (game_ipc.h) and a data directory of its own. SDL games need
 * no change: the SDK's SDL2 (APPS_CONFIG bare-app.SDL=y) has the
 * "bareapp" video and joystick drivers, which draw into bare_app's video
 * layer and turn its gamepad into a game controller and, as the ini says,
 * keys. docs/game.ini describes the file.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "core/log.h"
#include "game.h"
#include "ini.h"

#define TAG "native"
#define MAX_GAMES 32
#define MAX_NEEDS 4
#define MAX_ENV 16
#define MAX_ARGS 32
#define STR 256

struct native_game {
	char id[64];			/* the ini's name: the data directory */
	char name[64];
	char dir[STR];			/* the ini's directory: relative paths */
	char exe[STR], args[STR], cwd[STR];
	char needs[MAX_NEEDS][STR];
	int n_needs;
	char hint[512], hint_ru[512];
	bool trigger_volume;
	/* SDL_BAREAPP_*: keys, gamepad, scale, filter, fps */
	char keys[STR], gamepad[STR], scale[16], filter[16], fps[8];
	char env[MAX_ENV][STR];		/* "NAME=value" of [env] */
	int n_env;
};

static struct native_game *found;
static int count;

#define COPY(dst, src) snprintf(dst, sizeof(dst), "%s", src)

static bool yes(const char *v)
{
	return !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1");
}

/* A hint's "\n" as a new line */
static void copy_lines(char *dst, size_t size, const char *src)
{
	size_t n = 0;

	for (; *src && n + 1 < size; src++)
		if (src[0] == '\\' && src[1] == 'n') {
			dst[n++] = '\n';
			src++;
		} else {
			dst[n++] = *src;
		}
	dst[n] = '\0';
}

static bool ini_value(const char *section, const char *key, const char *value, void *ud)
{
	struct native_game *g = ud;

	if (!strcasecmp(section, "env")) {
		if (g->n_env < MAX_ENV)
			snprintf(g->env[g->n_env++], STR, "%s=%s", key, value);
		return true;
	}
	if (*section)
		return true;
	if (!strcasecmp(key, "name"))
		COPY(g->name, value);
	else if (!strcasecmp(key, "exe"))
		COPY(g->exe, value);
	else if (!strcasecmp(key, "args"))
		COPY(g->args, value);
	else if (!strcasecmp(key, "dir"))
		COPY(g->cwd, value);
	else if (!strcasecmp(key, "needs") && g->n_needs < MAX_NEEDS)
		COPY(g->needs[g->n_needs++], value);
	else if (!strcasecmp(key, "hint"))
		copy_lines(g->hint, sizeof(g->hint), value);
	else if (!strcasecmp(key, "hint.ru"))
		copy_lines(g->hint_ru, sizeof(g->hint_ru), value);
	else if (!strcasecmp(key, "volume_triggers"))
		g->trigger_volume = yes(value);
	else if (!strcasecmp(key, "keys"))
		COPY(g->keys, value);
	else if (!strcasecmp(key, "gamepad"))
		COPY(g->gamepad, value);
	else if (!strcasecmp(key, "scale"))
		COPY(g->scale, value);
	else if (!strcasecmp(key, "filter"))
		COPY(g->filter, value);
	else if (!strcasecmp(key, "fps"))
		COPY(g->fps, value);
	else
		LOGW(TAG, "%s: unknown key \"%s\"", g->id, key);
	return true;
}

/* One more game from path (in dir), named id unless the ini names it */
static void add(const char *path, const char *dir, const char *id)
{
	struct native_game *g;

	if (count == MAX_GAMES)
		return;
	g = &found[count];
	memset(g, 0, sizeof(*g));
	COPY(g->id, id);
	COPY(g->dir, dir);
	if (ini_read(path, ini_value, g) < 0)
		return;
	if (!g->exe[0]) {
		LOGW(TAG, "%s: no exe", path);
		return;
	}
	if (!g->name[0])
		COPY(g->name, id);
	count++;
}

static void scan_dir(const char *root)
{
	char path[PATH_MAX + 16], sub[PATH_MAX], id[64];
	struct dirent *e;
	DIR *d = opendir(root);

	while (d && (e = readdir(d))) {
		size_t len = strlen(e->d_name);

		if (e->d_name[0] == '.')
			continue;
		/* <root>/<id>.ini: the program is elsewhere (a package's) */
		if (len > 4 && !strcasecmp(e->d_name + len - 4, ".ini")) {
			snprintf(id, sizeof(id), "%.*s", (int)(len - 4), e->d_name);
			snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
			add(path, root, id);
			continue;
		}
		/* <root>/<id>/game.ini: the game in its directory */
		snprintf(sub, sizeof(sub), "%s/%s", root, e->d_name);
		snprintf(path, sizeof(path), "%s/game.ini", sub);
		if (access(path, R_OK) == 0) {
			snprintf(id, sizeof(id), "%s", e->d_name);
			add(path, sub, id);
		}
	}
	if (d)
		closedir(d);
}

static int by_name(const void *a, const void *b)
{
	return strcasecmp(((const struct native_game *)a)->name, ((const struct native_game *)b)->name);
}

static int native_scan(void)
{
	char dirs[PATH_MAX], *dir, *save = NULL;

	if (!found) {
		found = calloc(MAX_GAMES, sizeof(*found));
		if (!found)
			return 0;
	}
	count = 0;
	snprintf(dirs, sizeof(dirs), "%s", game_setting("BARE_APP_NATIVE_DIRS", CONFIG_GAME_NATIVE_DIRS));
	for (dir = strtok_r(dirs, " :", &save); dir; dir = strtok_r(NULL, " :", &save))
		scan_dir(dir);
	qsort(found, count, sizeof(found[0]), by_name);
	return count;
}

static int native_entries(bool rescan)
{
	return rescan ? native_scan() : count;
}

static void native_info(int i, struct game_info *gi)
{
	const struct native_game *g;

	if (i < 0 || i >= count)
		return;
	g = &found[i];
	gi->name = g->name;
	gi->hint = !strncmp(i18n_lang(), "ru", 2) && g->hint_ru[0] ? g->hint_ru : g->hint[0] ? g->hint : NULL;
	gi->trigger_volume = g->trigger_volume;
}

/* path of the game: as it is if absolute, else in the ini's directory */
static void resolve(const struct native_game *g, const char *path, char *buf, size_t size)
{
	int n = path[0] == '/' ? snprintf(buf, size, "%s", path)
			       : snprintf(buf, size, "%s/%s", g->dir, path);

	if (n < 0 || (size_t)n >= size)
		LOGW(TAG, "%s: path too long", path);
}

static const char *native_missing(int i)
{
	static char path[PATH_MAX];
	const struct native_game *g;

	if (i < 0 || i >= count)
		return game_setting("BARE_APP_NATIVE_DIRS", CONFIG_GAME_NATIVE_DIRS);
	g = &found[i];
	resolve(g, g->exe, path, sizeof(path));
	if (access(path, X_OK) < 0)
		return path;
	for (int n = 0; n < g->n_needs; n++) {
		resolve(g, g->needs[n], path, sizeof(path));
		if (access(path, R_OK) < 0)
			return path;
	}
	return NULL;
}

/* {width} {height} {lang} {data} {dir} in s, into buf */
static void expand(const char *s, const struct game_env *ge, const char *data, const char *dir,
		   char *buf, size_t size)
{
	size_t n = 0;

	while (*s && n + 1 < size) {
		const char *close = *s == '{' ? strchr(s, '}') : NULL;
		char value[PATH_MAX] = "";
		size_t len = close ? (size_t)(close - s - 1) : 0;

		if (close && len == 5 && !strncmp(s + 1, "width", 5))
			snprintf(value, sizeof(value), "%d", ge->width);
		else if (close && len == 6 && !strncmp(s + 1, "height", 6))
			snprintf(value, sizeof(value), "%d", ge->height);
		else if (close && len == 4 && !strncmp(s + 1, "lang", 4))
			snprintf(value, sizeof(value), "%s", ge->lang);
		else if (close && len == 4 && !strncmp(s + 1, "data", 4))
			snprintf(value, sizeof(value), "%s", data);
		else if (close && len == 3 && !strncmp(s + 1, "dir", 3))
			snprintf(value, sizeof(value), "%s", dir);
		else
			close = NULL;
		if (!close) {
			buf[n++] = *s++;
			continue;
		}
		n += snprintf(buf + n, size - n, "%s", value);
		if (n >= size)
			n = size - 1;
		s = close + 1;
	}
	buf[n] = '\0';
}

/* args into argv (words; "double quotes" keep spaces), in buf */
static int split(char *buf, char **argv, int max)
{
	int n = 0;
	char *s = buf, *out;

	while (*s && n < max) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		argv[n++] = out = s;
		while (*s && *s != ' ' && *s != '\t') {
			if (*s == '"') {
				for (s++; *s && *s != '"'; s++)
					*out++ = *s;
				if (*s)
					s++;
			} else {
				*out++ = *s++;
			}
		}
		if (*s)
			s++;
		*out = '\0';
	}
	return n;
}

static struct game_proc *native_start(int i, const struct game_env *ge)
{
	char exe[PATH_MAX], data[PATH_MAX], cwd[PATH_MAX], args[PATH_MAX], alsa[32];
	char *argv[MAX_ARGS + 2];
	struct game_vars env = { 0 };
	const char *missing = native_missing(i);
	const struct native_game *g;
	int n = 0;

	if (missing) {
		LOGE(TAG, "%s: %s", missing, strerror(ENOENT));
		errno = ENOENT;
		return NULL;
	}
	g = &found[i];
	resolve(g, g->exe, exe, sizeof(exe));
	snprintf(cwd, sizeof(cwd), "%s/%s", game_setting("BARE_APP_NATIVE_DATA", CONFIG_GAME_NATIVE_DATA), g->id);
	if (game_proc_mkdir(cwd, data, sizeof(data)) < 0) {
		LOGE(TAG, "%s: %s", cwd, strerror(errno));
		return NULL;
	}
	if (g->cwd[0]) {
		expand(g->cwd, ge, data, g->dir, args, sizeof(args));
		resolve(g, args, cwd, sizeof(cwd));
	} else {
		snprintf(cwd, sizeof(cwd), "%s", data);
	}

	argv[n++] = (char *)g->id;
	expand(g->args, ge, data, g->dir, args, sizeof(args));
	n += split(args, argv + n, MAX_ARGS);
	argv[n] = NULL;

	/* the game's settings and saved games in its data directory */
	game_vars_add(&env, "HOME=%s", data);
	game_vars_add(&env, "XDG_DATA_HOME=%s/.local/share", data);
	game_vars_add(&env, "XDG_CONFIG_HOME=%s/.config", data);
	game_vars_add(&env, "XDG_CACHE_HOME=%s/.cache", data);
	game_vars_add(&env, "BARE_APP_LANG=%s", ge->lang);
	game_vars_add(&env, "SDL_VIDEODRIVER=bareapp");
	/* Mesa's panfrost without AFBC: see GLES_ENV in halflife.c */
	game_vars_add(&env, "PAN_MESA_DEBUG=noafbc");
	game_vars_sdl_audio(&env, ge->pcm, alsa, sizeof(alsa));
	if (g->keys[0])
		game_vars_add(&env, "SDL_BAREAPP_KEYS=%s", g->keys);
	if (g->gamepad[0])
		game_vars_add(&env, "SDL_BAREAPP_GAMEPAD=%s", g->gamepad);
	if (g->scale[0])
		game_vars_add(&env, "SDL_BAREAPP_SCALE=%s", g->scale);
	if (g->filter[0])
		game_vars_add(&env, "SDL_BAREAPP_FILTER=%s", g->filter);
	if (g->fps[0])
		game_vars_add(&env, "SDL_BAREAPP_FPS=%s", g->fps);
	for (int k = 0; k < g->n_env; k++) {
		expand(g->env[k], ge, data, g->dir, args, sizeof(args));
		game_vars_add(&env, "%s", args);
	}

	struct game_spawn s = { .path = exe, .argv = argv, .env = env.v, .dir = cwd,
				.width = ge->width, .height = ge->height };

	LOGI(TAG, "%s: %s, data %s, sound %s", g->name, exe, data, alsa[0] ? alsa : "off");
	return game_proc_start(&s);
}

const struct game_def game_native = {
	.id = "native",
	.hint = S_NATIVE_HINT,
	.entries = native_entries,
	.info = native_info,
	.missing = native_missing,
	.start = native_start,
};
