/*
 * bare_app_wine: a Windows game under Wine as a game of bare_app.
 *
 *   bare_app_wine <game directory>
 *
 * Started by bare_app's src/games/wine.c with the socket and the frame
 * memory of game_ipc.h. Reads <game directory>/wine.ini (see wine.ini in
 * this directory), prepares the Wine prefix ($BARE_APP_WINE_PREFIX, made
 * by wineboot on the first start; drive D: is the parent of the game
 * directory) and starts Wine with WINEBARE_SOCKET. The winebare.drv driver
 * of every Wine process connects there and hands over the pixels of its
 * windows (winebare_proto.h); they are put together into one Windows
 * screen of the display mode, the cursor drawn over it, and the screen
 * scaled into bare_app's frame. bare_app's buttons and sticks become mouse
 * and keyboard input after the [keys] of wine.ini.
 *
 * Only pixels and input cross over: no code of Wine is linked here.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core/input.h"
#include "games/game_ipc_client.h"
#include "winebare_proto.h"

extern char **environ;

#define MAX_CLIENTS 32
#define MAX_ENV 32
#define TICK_NS (1000000000L / 60)	/* frames and cursor moves at most 60 a second */
#define STICK_DEAD 0.15f		/* stick deflection taken as rest */
#define WINEBOOT_ENV "WINEDLLOVERRIDES=mscoree,mshtml="	/* no Mono or Gecko dialogs */

static void msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("bare_app_wine: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ---- wine.ini ------------------------------------------------------------ */

enum act_kind { ACT_NONE, ACT_KEYS, ACT_MOUSE, ACT_WHEEL };

/* What a button does: up to 4 keys pressed together (ctrl+s), a mouse
 * button (the WB_MOUSE_*_DOWN flag) or a wheel step (+1 / -1) */
struct action {
	uint8_t kind, count;
	int16_t wheel;
	uint32_t mouse;
	uint16_t vk[4];
};

enum stick_use { STICK_NONE, STICK_MOUSE, STICK_ARROWS, STICK_WHEEL, STICK_NUMPAD, STICK_TURN };

static struct {
	char dir[PATH_MAX];		/* game directory */
	char exe[PATH_MAX];		/* relative to it */
	char args[1024];
	struct wb_mode mode;		/* the desktop's display mode */
	struct wb_mode modes[WB_MAX_MODES];
	unsigned mode_count;
	char dlls[512];			/* WINEDLLOVERRIDES */
	char locale[32];
	char env[1024];			/* NAME=value, space separated */
	char writable[512];		/* entries the game writes to, see make_tree() */
	enum stick_use lstick, rstick, dpad;
	bool cursor_keys;		/* arrows = cursor: the arrow keys are the cursor block's */
	float cursor_speed;		/* screen widths a second at full deflection */
	float mouse_speed;		/* desktop pixels a count of a real mouse */
	struct action button[GP_COUNT];
} cfg;

static const struct { const char *name; uint16_t vk; } key_names[] = {
	{ "backspace", 0x08 }, { "tab", 0x09 }, { "enter", 0x0d }, { "shift", 0x10 },
	{ "ctrl", 0x11 }, { "alt", 0x12 }, { "pause", 0x13 }, { "capslock", 0x14 },
	{ "esc", 0x1b }, { "space", 0x20 }, { "pgup", 0x21 }, { "pgdn", 0x22 },
	{ "end", 0x23 }, { "home", 0x24 }, { "left", 0x25 }, { "up", 0x26 },
	{ "right", 0x27 }, { "down", 0x28 }, { "insert", 0x2d }, { "delete", 0x2e },
	{ "kp*", 0x6a }, { "kp+", 0x6b }, { "kp-", 0x6d }, { "kp/", 0x6f },
	{ "lshift", 0xa0 }, { "rshift", 0xa1 }, { "lctrl", 0xa2 }, { "rctrl", 0xa3 },
	{ "lalt", 0xa4 }, { "ralt", 0xa5 }, { "-", 0xbd }, { "=", 0xbb }, { ",", 0xbc },
	{ ".", 0xbe }, { "/", 0xbf }, { "`", 0xc0 }, { "[", 0xdb }, { "]", 0xdd },
	{ ";", 0xba }, { "'", 0xde }, { "\\", 0xdc },
};

static const char *const button_names[GP_COUNT] = {
	[GP_UP] = "up", [GP_DOWN] = "down", [GP_LEFT] = "left", [GP_RIGHT] = "right",
	[GP_A] = "a", [GP_B] = "b", [GP_X] = "x", [GP_Y] = "y",
	[GP_L1] = "l1", [GP_R1] = "r1", [GP_L2] = "l2", [GP_R2] = "r2",
	[GP_SELECT] = "select", [GP_START] = "start", [GP_MENU] = "menu",
	[GP_F1] = "f1", [GP_F2] = "f2", [GP_F3] = "f3",
	[GP_F4] = "f4", [GP_F5] = "f5", [GP_F6] = "f6",
	[GP_VOL_UP] = "vol+", [GP_VOL_DOWN] = "vol-", [GP_POWER] = "power",
};

/*
 * The keypad as a PC keyboard with NumLock off sends it, which is how
 * Wine's desktop starts: kp8 is VK_UP with the keypad's scan code (0x48,
 * not extended; winebare.drv takes the first scan code of a VK). Pirates!
 * reads its keys so: it maps the keypad's scan codes to VKs with
 * MapVirtualKey and looks at those (VK_NUMPAD8 never moves anything).
 */
/* In an action's keys: the keypad's key of that VK whatever "arrows" says */
#define VK_KEYPAD 0x100

static const uint16_t keypad_vk[10] = {
	0x2d, 0x23, 0x28, 0x22, 0x25, 0x0c, 0x27, 0x24, 0x26, 0x21,	/* Ins End Down PgDn Left Clear Right Home Up PgUp */
};

static int key_vk(const char *name)
{
	size_t i;
	int n;

	if (!name[1] && isalnum((unsigned char)name[0]))
		return toupper((unsigned char)name[0]);	/* VK_0..9, VK_A..Z */
	if ((name[0] == 'f' || name[0] == 'F') && (n = atoi(name + 1)) >= 1 && n <= 24)
		return 0x70 + n - 1;
	if (!strncmp(name, "kp", 2) && isdigit((unsigned char)name[2]) && !name[3])
		return keypad_vk[name[2] - '0'] | VK_KEYPAD;
	if (!strcmp(name, "kp."))
		return 0x2e | VK_KEYPAD;	/* Del */
	for (i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++)
		if (!strcasecmp(name, key_names[i].name))
			return key_names[i].vk;
	return -1;
}

/* "lbutton", "wheelup", "esc", "ctrl+s", "none" */
static int parse_action(const char *value, struct action *a)
{
	char buf[64], *p, *save = NULL;
	int vk;

	memset(a, 0, sizeof(*a));
	if (!strcmp(value, "none"))
		return 0;
	if (!strcmp(value, "lbutton") || !strcmp(value, "rbutton") || !strcmp(value, "mbutton")) {
		a->kind = ACT_MOUSE;
		a->mouse = value[0] == 'l' ? WB_MOUSE_LEFT_DOWN :
			   value[0] == 'r' ? WB_MOUSE_RIGHT_DOWN : WB_MOUSE_MIDDLE_DOWN;
		return 0;
	}
	if (!strcmp(value, "wheelup") || !strcmp(value, "wheeldown")) {
		a->kind = ACT_WHEEL;
		a->wheel = value[5] == 'u' ? 1 : -1;
		return 0;
	}
	snprintf(buf, sizeof(buf), "%s", value);
	a->kind = ACT_KEYS;
	/* "+" alone is a key name too: kp+ */
	for (p = strtok_r(buf, "+", &save); p; p = strtok_r(NULL, "+", &save)) {
		if ((vk = key_vk(p)) < 0 || a->count == 4)
			return -1;
		a->vk[a->count++] = vk;
	}
	return a->count ? 0 : -1;
}

static int parse_mode(const char *s, struct wb_mode *m)
{
	unsigned w, h;

	if (sscanf(s, "%ux%u", &w, &h) != 2 || w < 320 || h < 200 || w > 4096 || h > 4096)
		return -1;
	m->width = w;
	m->height = h;
	return 0;
}

static void add_mode(struct wb_mode m)
{
	unsigned i;

	for (i = 0; i < cfg.mode_count; i++)
		if (cfg.modes[i].width == m.width && cfg.modes[i].height == m.height)
			return;
	if (cfg.mode_count < WB_MAX_MODES)
		cfg.modes[cfg.mode_count++] = m;
}

static enum stick_use parse_stick(const char *v)
{
	return !strcmp(v, "mouse") ? STICK_MOUSE : !strcmp(v, "arrows") ? STICK_ARROWS :
	       !strcmp(v, "wheel") ? STICK_WHEEL : !strcmp(v, "numpad") ? STICK_NUMPAD :
	       !strcmp(v, "turn") ? STICK_TURN : STICK_NONE;
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

static void set_defaults(void)
{
	static const char *const keys[][2] = {
		{ "up", "up" }, { "down", "down" }, { "left", "left" }, { "right", "right" },
		{ "a", "lbutton" }, { "b", "rbutton" }, { "x", "enter" }, { "y", "space" },
		{ "l1", "pgup" }, { "r1", "pgdn" }, { "l2", "shift" }, { "r2", "ctrl" },
		{ "select", "tab" }, { "start", "esc" },
		{ "f1", "f1" }, { "f2", "f2" }, { "f3", "f3" }, { "f4", "f4" },
	};
	static const struct wb_mode modes[] = { { 640, 480 }, { 800, 600 }, { 1024, 768 } };
	size_t i;
	int b;

	cfg.mode = (struct wb_mode){ 800, 600 };
	cfg.lstick = STICK_MOUSE;
	cfg.rstick = STICK_WHEEL;
	cfg.dpad = STICK_ARROWS;
	cfg.cursor_speed = 0.8f;
	cfg.mouse_speed = 1.0f;
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
		add_mode(modes[i]);
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		for (b = 0; b < GP_COUNT; b++)
			if (button_names[b] && !strcmp(button_names[b], keys[i][0]))
				parse_action(keys[i][1], &cfg.button[b]);
}

static int read_ini(const char *path)
{
	char line[1024], *key, *value, *eq, *c;
	bool in_keys = false;
	int lineno = 0, b;
	FILE *f = fopen(path, "r");

	if (!f) {
		msg("%s: %s", path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		lineno++;
		/* comments: ";" or "#" at the start or after a blank */
		for (c = line; *c; c++)
			if ((*c == ';' || *c == '#') && (c == line || isspace((unsigned char)c[-1]))) {
				*c = '\0';
				break;
			}
		key = trim(line);
		if (!*key)
			continue;
		if (*key == '[') {
			in_keys = !strcasecmp(key, "[keys]");
			continue;
		}
		if (!(eq = strchr(key, '='))) {
			msg("%s:%d: no \"=\"", path, lineno);
			continue;
		}
		*eq = '\0';
		value = trim(eq + 1);
		key = trim(key);
		for (c = key; *c; c++)
			*c = tolower((unsigned char)*c);
		if (in_keys) {
			if (!strcmp(key, "stick"))
				cfg.lstick = parse_stick(value);
			else if (!strcmp(key, "rstick"))
				cfg.rstick = parse_stick(value);
			else if (!strcmp(key, "dpad"))
				cfg.dpad = parse_stick(value) == STICK_MOUSE ? STICK_MOUSE : STICK_ARROWS;
			else if (!strcmp(key, "cursor_speed"))
				cfg.cursor_speed = strtof(value, NULL);
			else if (!strcmp(key, "mouse_speed"))
				cfg.mouse_speed = strtof(value, NULL);
			else if (!strcmp(key, "arrows"))
				cfg.cursor_keys = !strcmp(value, "cursor");
			else {
				for (b = 0; b < GP_COUNT; b++)
					if (button_names[b] && !strcmp(button_names[b], key))
						break;
				if (b == GP_COUNT || parse_action(value, &cfg.button[b]) < 0)
					msg("%s:%d: %s = %s?", path, lineno, key, value);
			}
		} else if (!strcmp(key, "exe")) {
			snprintf(cfg.exe, sizeof(cfg.exe), "%s", value);
		} else if (!strcmp(key, "args")) {
			snprintf(cfg.args, sizeof(cfg.args), "%s", value);
		} else if (!strcmp(key, "mode")) {
			if (parse_mode(value, &cfg.mode) < 0)
				msg("%s:%d: mode %s?", path, lineno, value);
		} else if (!strcmp(key, "modes")) {
			char *save = NULL, *m;
			struct wb_mode wm;

			for (m = strtok_r(value, " ,", &save); m; m = strtok_r(NULL, " ,", &save))
				if (parse_mode(m, &wm) == 0)
					add_mode(wm);
		} else if (!strcmp(key, "dlls")) {
			snprintf(cfg.dlls, sizeof(cfg.dlls), "%s", value);
		} else if (!strcmp(key, "env")) {
			snprintf(cfg.env, sizeof(cfg.env), "%s", value);
		} else if (!strcmp(key, "writable")) {
			snprintf(cfg.writable, sizeof(cfg.writable), "%s", value);
		} else if (!strcmp(key, "locale")) {
			snprintf(cfg.locale, sizeof(cfg.locale), "%s", value);
		} else if (strcmp(key, "name") && strcmp(key, "render")) {
			msg("%s:%d: %s?", path, lineno, key);
		}
	}
	fclose(f);
	add_mode(cfg.mode);
	if (!cfg.exe[0]) {
		msg("%s: no exe", path);
		return -1;
	}
	return 0;
}

/* ---- processes ------------------------------------------------------------ */

static const char *wine_bin(const char *name, char *buf, size_t size)
{
	const char *dir = getenv("BARE_APP_WINE_BIN");

	snprintf(buf, size, "%s/%s", dir && *dir ? dir : "/usr/bin", name);
	return buf;
}

/* argv[0] of Wine's bin directory with our environment and EXTRA added */
static pid_t spawn(char *const argv[], const char *dir, const char *extra)
{
	posix_spawn_file_actions_t fa;
	char path[PATH_MAX], *env[256];
	size_t n = 0, i;
	pid_t pid;
	int err;

	for (i = 0; environ[i] && n < 254; i++)
		if (!extra || strncmp(environ[i], extra, strchr(extra, '=') - extra + 1))
			env[n++] = environ[i];
	if (extra)
		env[n++] = (char *)extra;
	env[n] = NULL;
	posix_spawn_file_actions_init(&fa);
	if (dir)
		posix_spawn_file_actions_addchdir_np(&fa, dir);
	err = posix_spawn(&pid, wine_bin(argv[0], path, sizeof(path)), &fa, NULL, argv, env);
	posix_spawn_file_actions_destroy(&fa);
	if (err) {
		msg("%s: %s", path, strerror(err));
		return -1;
	}
	return pid;
}

static int run(char *const argv[], const char *extra)
{
	int status;
	pid_t pid = spawn(argv, NULL, extra);

	if (pid < 0)
		return -1;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int mkdirs(const char *path)
{
	char buf[PATH_MAX], *p;

	snprintf(buf, sizeof(buf), "%s", path);
	for (p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(buf, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

static void set_env(void)
{
	const char *prefix = getenv("BARE_APP_WINE_PREFIX");
	char buf[sizeof(cfg.env)], *p, *save = NULL, lang[48];

	setenv("WINEPREFIX", prefix && *prefix ? prefix : "/data/wine/prefix", 1);
	setenv("WINEDEBUG", "-all,err+all", 0);
	/* the 32-bit programs only: no 64-bit Windows dlls to look for */
	setenv("WINEARCH", "win64", 0);
	if (cfg.locale[0]) {
		snprintf(lang, sizeof(lang), "%s.UTF-8", cfg.locale);
		setenv("LANG", lang, 1);
		setenv("LC_ALL", lang, 1);
	}
	if (cfg.dlls[0])
		setenv("WINEDLLOVERRIDES", cfg.dlls, 1);
	snprintf(buf, sizeof(buf), "%s", cfg.env);
	for (p = strtok_r(buf, " \t", &save); p; p = strtok_r(NULL, " \t", &save)) {
		char *eq = strchr(p, '=');

		if (!eq || eq == p) {
			msg("env %s?", p);
			continue;
		}
		*eq = '\0';
		setenv(p, eq + 1, 1);
	}
}

/*
 * The prefix made on the first start, drive D: the directory of the
 * games. The windows of wineboot are shown like the game's.
 */
static int prepare_prefix(void)
{
	const char *prefix = getenv("WINEPREFIX");
	char path[PATH_MAX], games[PATH_MAX], *slash;
	char *const wineboot[] = { (char *)"wine", (char *)"wineboot", (char *)"-i", NULL };
	struct stat st;

	if (mkdirs(prefix) < 0) {
		msg("%s: %s", prefix, strerror(errno));
		return -1;
	}
	snprintf(path, sizeof(path), "%s/system.reg", prefix);
	if (stat(path, &st) < 0) {
		struct timespec t0, t1;

		msg("making the prefix %s", prefix);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (run(wineboot, WINEBOOT_ENV) != 0 || stat(path, &st) < 0) {
			msg("wineboot failed");
			return -1;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		msg("prefix made in %ld s", (long)(t1.tv_sec - t0.tv_sec));
	}
	snprintf(games, sizeof(games), "%s", cfg.dir);
	slash = strrchr(games, '/');
	if (slash && slash != games)
		*slash = '\0';
	snprintf(path, sizeof(path), "%s/dosdevices/d:", prefix);
	unlink(path);
	if (symlink(games, path) < 0)
		msg("%s: %s", path, strerror(errno));
	return 0;
}

/* NAME is one of the "writable" entries (blank or comma separated, any case) */
static bool writable(const char *name)
{
	char buf[sizeof(cfg.writable)], *p, *save = NULL;

	snprintf(buf, sizeof(buf), "%s", cfg.writable);
	for (p = strtok_r(buf, " \t,", &save); p; p = strtok_r(NULL, " \t,", &save))
		if (!strcasecmp(p, name))
			return true;
	return false;
}

static int copy_tree(const char *src, const char *dst)
{
	struct stat st;

	if (lstat(src, &st) < 0)
		return -1;
	if (S_ISDIR(st.st_mode)) {
		DIR *d;
		struct dirent *e;
		int ret = 0;

		if (mkdir(dst, 0755) < 0 && errno != EEXIST)
			return -1;
		if (!(d = opendir(src)))
			return -1;
		while ((e = readdir(d))) {
			char s2[PATH_MAX], d2[PATH_MAX];

			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			snprintf(s2, sizeof(s2), "%s/%s", src, e->d_name);
			snprintf(d2, sizeof(d2), "%s/%s", dst, e->d_name);
			if (copy_tree(s2, d2) < 0)
				ret = -1;
		}
		closedir(d);
		return ret;
	}
	if (S_ISREG(st.st_mode)) {
		char buf[65536];
		ssize_t n = 0;
		int in = open(src, O_RDONLY | O_CLOEXEC), out;

		if (in < 0)
			return -1;
		out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (out < 0) {
			close(in);
			return -1;
		}
		while ((n = read(in, buf, sizeof(buf))) > 0)
			if (write(out, buf, (size_t)n) != n) {
				n = -1;
				break;
			}
		close(in);
		if (close(out) < 0 || n < 0)
			return -1;
	}
	return 0;
}

/*
 * Games that write into their own directory ("writable = Data" in wine.ini:
 * saved games, settings, logs) are started from a tree next to the prefix,
 * <prefix>/../games/<directory>: the writable entries are copies made on the
 * first start (kept from then on), everything else links to the game's
 * directory, which stays read-only on /games.
 */
static int make_tree(void)
{
	const char *prefix = getenv("WINEPREFIX"), *name = strrchr(cfg.dir, '/');
	char tree[PATH_MAX], src[PATH_MAX], dst[PATH_MAX], *slash;
	struct dirent *e;
	struct stat st;
	DIR *d;

	if (!cfg.writable[0])
		return 0;
	snprintf(tree, sizeof(tree), "%s", prefix);
	if ((slash = strrchr(tree, '/')))
		*slash = '\0';
	snprintf(tree + strlen(tree), sizeof(tree) - strlen(tree), "/games/%s", name ? name + 1 : cfg.dir);
	if (mkdirs(tree) < 0 || !(d = opendir(cfg.dir))) {
		msg("%s: %s", tree, strerror(errno));
		return -1;
	}
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(src, sizeof(src), "%s/%s", cfg.dir, e->d_name);
		snprintf(dst, sizeof(dst), "%s/%s", tree, e->d_name);
		if (writable(e->d_name)) {
			if (lstat(dst, &st) == 0 && !S_ISLNK(st.st_mode))
				continue;	/* the game's own copy */
			unlink(dst);
			msg("copying %s to %s", src, dst);
			if (copy_tree(src, dst) < 0)
				msg("%s: %s", dst, strerror(errno));
		} else {
			unlink(dst);	/* follows the game's directory as it is now */
			if (symlink(src, dst) < 0)
				msg("%s: %s", dst, strerror(errno));
		}
	}
	closedir(d);
	snprintf(cfg.dir, sizeof(cfg.dir), "%s", tree);
	return 0;
}

/* ---- the screen ----------------------------------------------------------- */

struct client;

struct surface {
	struct client *client;
	uint32_t id;
	uint32_t width, height, buffers;
	uint8_t *map;
	size_t size;
	uint64_t hwnd;
	int32_t x, y;
	uint32_t shown_w, shown_h;
	uint32_t layer, buffer;
	bool placed, visible, flushed;
	struct surface *next;
};

struct client {
	int fd;
	pid_t pid;
	unsigned serial;	/* the oldest one sends the input */
};

static struct client clients[MAX_CLIENTS];
static unsigned client_count, client_serial;
static struct surface *surfaces;

static uint64_t zorder[60];
static unsigned zorder_count;

static struct wb_mode mode;		/* current */
static uint32_t *screen;		/* mode.width x mode.height */
static bool dirty;

static float cur_x, cur_y;		/* the cursor on the screen */
static bool cursor_shown = true;

static bool trace;			/* $BARE_APP_WINE_TRACE: cursor and input */

static struct game_link app;		/* bare_app: frames, gamepad */
static int dst_x, dst_y, dst_w, dst_h;	/* the screen in the frame */
static int32_t *xmap, *ymap;		/* source pixel << 8 | weight */

static void set_mode(struct wb_mode m)
{
	uint32_t *s = calloc((size_t)m.width * m.height, 4);
	int x, y;

	if (!s) {
		msg("no memory for %ux%u", m.width, m.height);
		return;
	}
	free(screen);
	screen = s;
	mode = m;
	/* the aspect kept, centred */
	dst_h = app.shm->height;
	dst_w = (int)((int64_t)dst_h * m.width / m.height);
	if (dst_w > (int)app.shm->width) {
		dst_w = app.shm->width;
		dst_h = (int)((int64_t)dst_w * m.height / m.width);
	}
	dst_x = (app.shm->width - dst_w) / 2;
	dst_y = (app.shm->height - dst_h) / 2;
	free(xmap);
	free(ymap);
	xmap = malloc(sizeof(*xmap) * dst_w);
	ymap = malloc(sizeof(*ymap) * dst_h);
	if (!xmap || !ymap) {
		msg("out of memory");
		exit(1);
	}
	/* the centre of each frame pixel on the screen, 24.8 fixed point */
	for (x = 0; x < dst_w; x++) {
		int32_t s8 = (int32_t)(((2 * x + 1) * (int64_t)m.width * 128) / dst_w) - 128;

		xmap[x] = s8 < 0 ? 0 : s8 > (int32_t)(m.width - 1) * 256 ? (int32_t)(m.width - 1) * 256 : s8;
	}
	for (y = 0; y < dst_h; y++) {
		int32_t s8 = (int32_t)(((2 * y + 1) * (int64_t)m.height * 128) / dst_h) - 128;

		ymap[y] = s8 < 0 ? 0 : s8 > (int32_t)(m.height - 1) * 256 ? (int32_t)(m.height - 1) * 256 : s8;
	}
	if (cur_x >= m.width)
		cur_x = m.width - 1;
	if (cur_y >= m.height)
		cur_y = m.height - 1;
	dirty = true;
	msg("display mode %ux%u, shown %dx%d", m.width, m.height, dst_w, dst_h);
}

static void blit(const struct surface *s)
{
	const uint8_t *src = s->map + (size_t)s->buffer * s->width * s->height * 4;
	int x0 = s->x, y0 = s->y, x1 = s->x + (int)s->shown_w, y1 = s->y + (int)s->shown_h;
	int y;

	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > mode.width)
		x1 = mode.width;
	if (y1 > mode.height)
		y1 = mode.height;
	if (x0 >= x1 || y0 >= y1)
		return;
	for (y = y0; y < y1; y++)
		memcpy(screen + (size_t)y * mode.width + x0,
		       src + ((size_t)(y - s->y) * s->width + (x0 - s->x)) * 4, (size_t)(x1 - x0) * 4);
}

static bool drawable(const struct surface *s)
{
	return s->placed && s->visible && s->flushed && s->map;
}

static int z_index(uint64_t hwnd)
{
	unsigned i;

	for (i = 0; i < zorder_count; i++)
		if (zorder[i] == hwnd)
			return i;
	return -1;
}

/* A standard arrow, 12 x 19: '#' outline, '.' fill */
static const char *const arrow[] = {
	"#           ", "##          ", "#.#         ", "#..#        ", "#...#       ",
	"#....#      ", "#.....#     ", "#......#    ", "#.......#   ", "#........#  ",
	"#.........# ", "#..........#", "#......#####", "#...#..#    ", "#..# #..#   ",
	"#.#  #..#   ", "##    #..#  ", "      #..#  ", "       ##   ",
};

static void draw_cursor(void)
{
	int cx = (int)cur_x, cy = (int)cur_y, x, y;
	/* drawn at the size it has on the device screen, ~19 pixels high */
	int scale = mode.height > (uint32_t)dst_h ? (mode.height + dst_h / 2) / dst_h : 1;

	for (y = 0; y < 19 * scale; y++) {
		if (cy + y >= (int)mode.height)
			break;
		for (x = 0; x < 12 * scale; x++) {
			char c = arrow[y / scale][x / scale];

			if (cx + x >= (int)mode.width)
				break;
			if (c != ' ')
				screen[(size_t)(cy + y) * mode.width + cx + x] = c == '#' ? 0 : 0xffffff;
		}
	}
}

static void compose(void)
{
	struct surface *s;
	int i;
	unsigned layer;

	memset(screen, 0, (size_t)mode.width * mode.height * 4);
	/* windows not in the z-order (yet): at the bottom */
	for (s = surfaces; s; s = s->next)
		if (drawable(s) && z_index(s->hwnd) < 0)
			blit(s);
	for (i = (int)zorder_count - 1; i >= 0; i--)
		for (layer = WB_LAYER_GDI; layer <= WB_LAYER_GL; layer++)
			for (s = surfaces; s; s = s->next)
				if (s->hwnd == zorder[i] && s->layer == layer && drawable(s))
					blit(s);
	if (cursor_shown)
		draw_cursor();
}

static inline uint32_t lerp(uint32_t a, uint32_t b, uint32_t f)
{
	uint32_t g = 256 - f;

	return ((((a & 0xff00ff) * g + (b & 0xff00ff) * f) >> 8) & 0xff00ff) |
	       ((((a & 0x00ff00) * g + (b & 0x00ff00) * f) >> 8) & 0x00ff00);
}

/* The screen into the frame being drawn, bilinear */
static void scale(void)
{
	uint8_t *frame = game_link_frame(&app);
	uint32_t w = mode.width, last_row = (uint32_t)-1;
	int x, y;

	for (y = 0; y < (int)app.shm->height; y++) {
		uint32_t *out = (uint32_t *)(frame + (size_t)y * app.shm->stride);

		if (y < dst_y || y >= dst_y + dst_h) {
			memset(out, 0, (size_t)app.shm->width * 4);
			continue;
		}
		memset(out, 0, (size_t)dst_x * 4);
		memset(out + dst_x + dst_w, 0, (size_t)(app.shm->width - dst_x - dst_w) * 4);
		out += dst_x;
		if ((uint32_t)ymap[y - dst_y] == last_row) {
			memcpy(out, (uint8_t *)out - app.shm->stride, (size_t)dst_w * 4);
			continue;
		}
		last_row = ymap[y - dst_y];

		uint32_t sy = last_row >> 8, fy = last_row & 0xff;
		const uint32_t *r0 = screen + (size_t)sy * w;
		const uint32_t *r1 = sy + 1 < mode.height ? r0 + w : r0;

		for (x = 0; x < dst_w; x++) {
			uint32_t sx = (uint32_t)xmap[x] >> 8, fx = xmap[x] & 0xff;
			uint32_t sx1 = sx + 1 < w ? sx + 1 : sx;

			out[x] = lerp(lerp(r0[sx], r0[sx1], fx), lerp(r1[sx], r1[sx1], fx), fy);
		}
	}
}

static int publish(void)
{
	compose();
	scale();
	dirty = false;
	return game_link_publish(&app);
}

/* ---- the Wine processes ------------------------------------------------------ */

static void client_send(struct client *c, const struct wb_msg *m)
{
	if (send(c->fd, m, sizeof(*m), MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EPIPE)
		msg("pid %d: message %u lost: %s", (int)c->pid, m->type, strerror(errno));
}

static void send_config(struct client *c)
{
	struct wb_msg m = { .type = WB_CONFIG };

	m.config.mode_count = cfg.mode_count;
	memcpy(m.config.modes, cfg.modes, sizeof(cfg.modes));
	m.config.current = mode;
	client_send(c, &m);
}

/* Input goes to wineserver through any process: the oldest one, usually
 * the desktop (explorer), which lives as long as Wine */
static struct client *input_client(void)
{
	struct client *best = NULL;
	unsigned i;

	for (i = 0; i < client_count; i++)
		if (!best || clients[i].serial < best->serial)
			best = &clients[i];
	return best;
}

/* The cursor to x,y: may be past the screen's edge (see move_tick()) */
static void send_mouse_at(int x, int y, uint32_t flags, int wheel)
{
	struct client *c = input_client();
	struct wb_msg m = { .type = WB_INPUT_MOUSE };

	if (!c)
		return;
	if (trace && (flags || wheel))
		msg("mouse %d,%d flags %#x wheel %d", x, y, flags, wheel);
	m.mouse.x = x;
	m.mouse.y = y;
	m.mouse.flags = flags | WB_MOUSE_MOVE;
	if (wheel) {
		m.mouse.flags |= WB_MOUSE_WHEEL;
		m.mouse.wheel = wheel;
	}
	client_send(c, &m);
}

static void send_mouse(uint32_t flags, int wheel)
{
	send_mouse_at((int)cur_x, (int)cur_y, flags, wheel);
}

/* A key of the cursor block, which the numeric keypad has too */
static bool cursor_block(uint16_t vk)
{
	return (vk >= 0x21 && vk <= 0x28) || vk == 0x2d || vk == 0x2e;	/* PgUp..Down, Ins, Del */
}

/* vk may have VK_KEYPAD */
static void send_key(uint16_t vk, bool pressed)
{
	struct client *c = input_client();
	struct wb_msg m = { .type = WB_INPUT_KEY };
	bool extended = !(vk & VK_KEYPAD) && cfg.cursor_keys && cursor_block(vk);

	if (trace)
		msg("key %#x%s %s%s", vk & 0xff, extended ? " (cursor block)" : "",
		    pressed ? "down" : "up", c ? "" : " (no window)");
	if (!c)
		return;
	m.key.vk = vk & 0xff;
	m.key.pressed = pressed;
	m.key.flags = extended ? WB_KEY_EXTENDED : 0;
	client_send(c, &m);
}

static struct surface *find_surface(struct client *c, uint32_t id)
{
	struct surface *s;

	for (s = surfaces; s; s = s->next)
		if (s->client == c && s->id == id)
			return s;
	return NULL;
}

static void free_surface(struct surface *s)
{
	struct surface **p;

	for (p = &surfaces; *p; p = &(*p)->next)
		if (*p == s) {
			*p = s->next;
			break;
		}
	if (s->map)
		munmap(s->map, s->size);
	if (drawable(s))
		dirty = true;
	free(s);
}

static void new_surface(struct client *c, const struct wb_msg *m, int fd)
{
	struct surface *s = find_surface(c, m->surface.id);
	size_t size = (size_t)m->surface.width * m->surface.height * 4 * m->surface.buffers;
	void *map;

	if (s)
		free_surface(s);
	if (fd < 0 || !size || m->surface.buffers > 2) {
		msg("pid %d: bad surface %u", (int)c->pid, m->surface.id);
		return;
	}
	map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED || !(s = calloc(1, sizeof(*s)))) {
		msg("pid %d: surface %u: %s", (int)c->pid, m->surface.id, strerror(errno));
		if (map != MAP_FAILED)
			munmap(map, size);
		return;
	}
	s->client = c;
	s->id = m->surface.id;
	s->width = m->surface.width;
	s->height = m->surface.height;
	s->buffers = m->surface.buffers;
	s->map = map;
	s->size = size;
	s->next = surfaces;
	surfaces = s;
}

static void on_client_msg(struct client *c, const struct wb_msg *m, int fd)
{
	struct surface *s;

	switch (m->type) {
	case WB_HELLO:
		c->pid = m->hello.pid;
		if (m->hello.version != WB_PROTO_VERSION)
			msg("pid %d: protocol %u, ours %u", (int)c->pid, m->hello.version, WB_PROTO_VERSION);
		send_config(c);
		break;
	case WB_SURFACE:
		new_surface(c, m, fd);
		break;
	case WB_SURFACE_DEL:
		if ((s = find_surface(c, m->surface_del.id)))
			free_surface(s);
		break;
	case WB_PLACE:
		if (!(s = find_surface(c, m->place.id)))
			break;
		if (drawable(s))
			dirty = true;
		s->hwnd = m->place.hwnd;
		s->x = m->place.x;
		s->y = m->place.y;
		s->shown_w = m->place.width < s->width ? m->place.width : s->width;
		s->shown_h = m->place.height < s->height ? m->place.height : s->height;
		s->visible = m->place.visible;
		s->layer = m->place.layer;
		s->placed = true;
		if (drawable(s))
			dirty = true;
		break;
	case WB_FLUSH:
		if (!(s = find_surface(c, m->flush.id)))
			break;
		s->buffer = m->flush.buffer < s->buffers ? m->flush.buffer : 0;
		s->flushed = true;
		if (drawable(s))
			dirty = true;
		break;
	case WB_ZORDER:
		zorder_count = m->zorder.count < 60 ? m->zorder.count : 60;
		memcpy(zorder, m->zorder.hwnd, zorder_count * sizeof(zorder[0]));
		dirty = true;
		break;
	case WB_MODE_SET: {
		struct wb_mode wm = { m->mode_set.width, m->mode_set.height };
		unsigned i;

		if (wm.width == mode.width && wm.height == mode.height)
			break;
		if (wm.width < 1 || wm.height < 1 || wm.width > 4096 || wm.height > 4096)
			break;
		set_mode(wm);
		for (i = 0; i < client_count; i++)
			if (&clients[i] != c)
				send_config(&clients[i]);
		break;
	}
	case WB_CURSOR:
		if (trace)
			msg("pid %d: cursor %s", (int)c->pid, m->cursor.visible ? "shown" : "hidden");
		if (cursor_shown != !!m->cursor.visible) {
			cursor_shown = m->cursor.visible;
			dirty = true;
		}
		break;
	case WB_CURSOR_POS:
		if (trace)
			msg("pid %d: cursor moved to %d,%d", (int)c->pid, m->cursor_pos.x, m->cursor_pos.y);
		cur_x = m->cursor_pos.x < 0 ? 0 : m->cursor_pos.x >= mode.width ? mode.width - 1 : m->cursor_pos.x;
		cur_y = m->cursor_pos.y < 0 ? 0 : m->cursor_pos.y >= mode.height ? mode.height - 1 : m->cursor_pos.y;
		dirty = cursor_shown;
		break;
	default:
		msg("pid %d: message %u?", (int)c->pid, m->type);
		break;
	}
	if (fd >= 0)
		close(fd);
}

static void drop_client(unsigned i)
{
	struct client *c = &clients[i];
	struct surface *s, *next;

	for (s = surfaces; s; s = next) {
		next = s->next;
		if (s->client == c)
			free_surface(s);
	}
	close(c->fd);
	/* surfaces point at their client: those of the last one move with it */
	if (i != client_count - 1) {
		clients[i] = clients[client_count - 1];
		for (s = surfaces; s; s = s->next)
			if (s->client == &clients[client_count - 1])
				s->client = c;
	}
	client_count--;
}

/* false once the client has gone */
static bool read_client(unsigned i)
{
	struct client *c = &clients[i];
	char control[CMSG_SPACE(sizeof(int))];
	struct wb_msg m;
	struct iovec iov = { &m, sizeof(m) };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
			     .msg_control = control, .msg_controllen = sizeof(control) };
	struct cmsghdr *cm;
	ssize_t n;
	int fd = -1;

	n = recvmsg(c->fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return true;
	if (n <= 0) {
		drop_client(i);
		return false;
	}
	for (cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
		if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS)
			memcpy(&fd, CMSG_DATA(cm), sizeof(fd));
	if ((size_t)n < sizeof(m.type)) {
		if (fd >= 0)
			close(fd);
		return true;
	}
	on_client_msg(c, &m, fd);
	return true;
}

static void accept_client(int listen_fd)
{
	int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);

	if (fd < 0)
		return;
	if (client_count == MAX_CLIENTS) {
		msg("too many Wine processes");
		close(fd);
		return;
	}
	clients[client_count++] = (struct client){ .fd = fd, .serial = client_serial++ };
}

/* ---- gamepad -> mouse and keyboard ----------------------------------------- */

static struct action held[GP_COUNT];	/* what a held button did */
static float axis[GP_AXIS_COUNT];
static bool dpad_dir[4];		/* dpad = mouse */
static float wheel_acc;
static struct timespec last_move;

static void do_action(const struct action *a, bool pressed)
{
	int i;

	switch (a->kind) {
	case ACT_KEYS:
		if (pressed)
			for (i = 0; i < a->count; i++)
				send_key(a->vk[i], true);
		else
			for (i = a->count - 1; i >= 0; i--)
				send_key(a->vk[i], false);
		break;
	case ACT_MOUSE:
		send_mouse(pressed ? a->mouse : a->mouse << 1, 0);	/* *_UP follows *_DOWN */
		break;
	case ACT_WHEEL:
		if (pressed)
			send_mouse(0, a->wheel);
		break;
	}
}

static void on_button(enum gp_button b, bool pressed, bool stick)
{
	static const uint16_t arrows[4] = { 0x26, 0x28, 0x25, 0x27 };	/* up down left right */

	if ((unsigned)b >= GP_COUNT)
		return;
	if (stick) {
		/* the stick as a D-pad: arrows only if asked for */
		if (cfg.lstick == STICK_ARROWS && b <= GP_RIGHT)
			send_key(arrows[b], pressed);
		return;
	}
	if (b <= GP_RIGHT && cfg.dpad == STICK_MOUSE) {
		dpad_dir[b] = pressed;
		return;
	}
	if (pressed) {
		if (held[b].kind != ACT_NONE)
			return;
		held[b] = cfg.button[b];
		do_action(&held[b], true);
	} else {
		do_action(&held[b], false);
		held[b].kind = ACT_NONE;
	}
}

static float curve(float v)
{
	float a = fabsf(v);

	if (a < STICK_DEAD)
		return 0;
	a = (a - STICK_DEAD) / (1 - STICK_DEAD);
	return copysignf(a * a, v);
}

/*
 * The right stick as the numeric keypad: its 8 directions are the keys
 * around 5 (up 8, up-right 9, right 6 ...), held while the stick is out.
 * Pushed out beyond NUMPAD_PRESS a key goes down; it changes only once
 * the stick turns clearly into another direction, and goes up once the
 * stick has stayed below NUMPAD_RELEASE for NUMPAD_REST_MS (a lone ADC
 * sample near the center is no release), but not before NUMPAD_MIN_MS:
 * a quick flick lasts at least a frame of the game.
 */
#define NUMPAD_PRESS 0.5f
#define NUMPAD_RELEASE 0.35f
#define NUMPAD_TURN (0.3927f + 0.17f)	/* half a direction (22.5 degrees) + 10 */
#define NUMPAD_MIN_MS 60
#define NUMPAD_REST_MS 20

static struct {
	int dir;		/* 0 right, then clockwise on the screen; -1 none */
	bool release;		/* back at rest, the key still down (move_tick() ends it) */
	struct timespec since, rest;
} numpad = { .dir = -1 };

static long ms_since(const struct timespec *t)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - t->tv_sec) * 1000 + (now.tv_nsec - t->tv_nsec) / 1000000;
}

static void numpad_set(int dir)
{
	/* right, down-right, down, down-left, left, up-left, up, up-right */
	static const uint8_t digit[8] = { 6, 3, 2, 1, 4, 7, 8, 9 };

	numpad.release = false;
	if (dir == numpad.dir)
		return;
	if (numpad.dir >= 0)
		send_key(keypad_vk[digit[numpad.dir]] | VK_KEYPAD, false);
	if (dir >= 0) {
		send_key(keypad_vk[digit[dir]] | VK_KEYPAD, true);
		clock_gettime(CLOCK_MONOTONIC, &numpad.since);
	}
	numpad.dir = dir;
}

/* After the messages from bare_app: both axes of a move are in */
static void numpad_update(void)
{
	float x = axis[GP_AXIS_RX], y = axis[GP_AXIS_RY], r = hypotf(x, y), a = atan2f(y, x);
	int dir = ((int)lrintf(a / 0.7854f) % 8 + 8) % 8;

	if (cfg.rstick != STICK_NUMPAD)
		return;
	if (numpad.dir < 0) {
		if (r > NUMPAD_PRESS)
			numpad_set(dir);
	} else if (r < NUMPAD_RELEASE) {
		if (!numpad.release) {
			numpad.release = true;
			clock_gettime(CLOCK_MONOTONIC, &numpad.rest);
		}
	} else if (dir != numpad.dir) {
		float d = fabsf(remainderf(a - (float)numpad.dir * 0.7854f, 6.2832f));

		if (d > NUMPAD_TURN)
			numpad_set(dir);
		else
			numpad.release = false;
	} else {
		numpad.release = false;
	}
}

/*
 * The right stick as a camera turned by the arrow keys and zoomed by the
 * wheel (rstick = turn: Daemonica turns its camera with Left and Right).
 * Left and right hold that key for a share of the time as big as the push
 * (after curve()), so a light push turns slowly: the push is owed as key
 * time, the key goes down once TURN_SLICE / 2 is owed and up once it has
 * been down that much too long, so it stays down or up for about
 * TURN_SLICE at least (a game looks at the key once a frame). Full out it
 * stays down. Up and down are the wheel, as "wheel".
 */
#define TURN_SLICE 0.08f	/* seconds */

static struct {
	float owed;		/* seconds of the key down the push asks for */
	uint16_t key;		/* down now, 0 none */
} turn;

static void turn_tick(float dt)
{
	float x = curve(axis[GP_AXIS_RX]);
	uint16_t want = x < 0 ? 0x25 : x > 0 ? 0x27 : 0;	/* VK_LEFT, VK_RIGHT */

	if (turn.key && turn.key != want) {
		send_key(turn.key, false);
		turn.key = 0;
		turn.owed = 0;
	}
	if (!want) {
		turn.owed = 0;
		return;
	}
	turn.owed += fabsf(x) * dt;
	if (turn.key) {
		turn.owed -= dt;
		if (turn.owed <= -TURN_SLICE / 2) {
			send_key(turn.key, false);
			turn.key = 0;
			turn.owed = 0;
		}
	} else if (turn.owed >= TURN_SLICE / 2) {
		send_key(want, true);
		turn.key = want;
		turn.owed = 0;
	}
}

static bool moving(void)
{
	if (numpad.release || turn.key)
		return true;
	return (cfg.lstick == STICK_MOUSE && (curve(axis[GP_AXIS_LX]) || curve(axis[GP_AXIS_LY]))) ||
	       (cfg.rstick == STICK_MOUSE && (curve(axis[GP_AXIS_RX]) || curve(axis[GP_AXIS_RY]))) ||
	       ((cfg.rstick == STICK_WHEEL || cfg.rstick == STICK_TURN) && curve(axis[GP_AXIS_RY])) ||
	       (cfg.rstick == STICK_TURN && curve(axis[GP_AXIS_RX])) ||
	       dpad_dir[0] || dpad_dir[1] || dpad_dir[2] || dpad_dir[3];
}

/* The cursor to nx, ny (the sticks, a mouse): clamped to the screen */
static void move_cursor(float nx, float ny)
{
	static float past_x, past_y;	/* pushed on past the screen's edge */
	int ox = (int)cur_x, oy = (int)cur_y, px, py;

	cur_x = nx < 0 ? 0 : nx > mode.width - 1 ? mode.width - 1 : nx;
	cur_y = ny < 0 ? 0 : ny > mode.height - 1 ? mode.height - 1 : ny;
	/*
	 * Pushed on at the edge, the cursor goes to a point past it: Wine
	 * keeps its own cursor in, but the move still reaches raw input. A
	 * game reading the mouse by DirectInput moves its own cursor by those
	 * moves only, and it is often not where this one is (Wine clips the
	 * cursor of an exclusive DirectInput mouse to the window instead of
	 * centring it): without this, its cursor stopped short of the
	 * screen's edge once this one got there first.
	 */
	past_x = nx != cur_x ? past_x + (nx - cur_x) : 0;
	past_y = ny != cur_y ? past_y + (ny - cur_y) : 0;
	px = (int)past_x;
	py = (int)past_y;
	if ((int)cur_x != ox || (int)cur_y != oy || px || py) {
		send_mouse_at((int)cur_x + px, (int)cur_y + py, 0, 0);
		past_x -= px;
		past_y -= py;
		if (cursor_shown)
			dirty = true;
	}
}

/* ---- a USB keyboard and mouse (GAME_MSG_KEY, GAME_MSG_MOUSE_*) ---------- */

/*
 * Linux key code -> Windows virtual key and PC scan code (set 1) of a US
 * keyboard. Below 89 the scan code is the key code itself; the rest are
 * the E0 keys. Shift, Ctrl, Alt are the generic VKs, as Windows reports
 * them, the scan code tells left from right.
 */
static const struct { uint8_t vk, scan; bool ext; } linux_keys[128] = {
	[1] = { 0x1b, 1 }, [2] = { '1', 2 }, [3] = { '2', 3 }, [4] = { '3', 4 }, [5] = { '4', 5 },
	[6] = { '5', 6 }, [7] = { '6', 7 }, [8] = { '7', 8 }, [9] = { '8', 9 }, [10] = { '9', 10 },
	[11] = { '0', 11 }, [12] = { 0xbd, 12 }, [13] = { 0xbb, 13 }, [14] = { 0x08, 14 },
	[15] = { 0x09, 15 }, [16] = { 'Q', 16 }, [17] = { 'W', 17 }, [18] = { 'E', 18 },
	[19] = { 'R', 19 }, [20] = { 'T', 20 }, [21] = { 'Y', 21 }, [22] = { 'U', 22 },
	[23] = { 'I', 23 }, [24] = { 'O', 24 }, [25] = { 'P', 25 }, [26] = { 0xdb, 26 },
	[27] = { 0xdd, 27 }, [28] = { 0x0d, 28 }, [29] = { 0x11, 29 }, [30] = { 'A', 30 },
	[31] = { 'S', 31 }, [32] = { 'D', 32 }, [33] = { 'F', 33 }, [34] = { 'G', 34 },
	[35] = { 'H', 35 }, [36] = { 'J', 36 }, [37] = { 'K', 37 }, [38] = { 'L', 38 },
	[39] = { 0xba, 39 }, [40] = { 0xde, 40 }, [41] = { 0xc0, 41 }, [42] = { 0x10, 42 },
	[43] = { 0xdc, 43 }, [44] = { 'Z', 44 }, [45] = { 'X', 45 }, [46] = { 'C', 46 },
	[47] = { 'V', 47 }, [48] = { 'B', 48 }, [49] = { 'N', 49 }, [50] = { 'M', 50 },
	[51] = { 0xbc, 51 }, [52] = { 0xbe, 52 }, [53] = { 0xbf, 53 }, [54] = { 0x10, 54 },
	[55] = { 0x6a, 55 }, [56] = { 0x12, 56 }, [57] = { 0x20, 57 }, [58] = { 0x14, 58 },
	[59] = { 0x70, 59 }, [60] = { 0x71, 60 }, [61] = { 0x72, 61 }, [62] = { 0x73, 62 },
	[63] = { 0x74, 63 }, [64] = { 0x75, 64 }, [65] = { 0x76, 65 }, [66] = { 0x77, 66 },
	[67] = { 0x78, 67 }, [68] = { 0x79, 68 }, [69] = { 0x90, 69 }, [70] = { 0x91, 70 },
	/* the keypad as with NumLock on */
	[71] = { 0x67, 71 }, [72] = { 0x68, 72 }, [73] = { 0x69, 73 }, [74] = { 0x6d, 74 },
	[75] = { 0x64, 75 }, [76] = { 0x65, 76 }, [77] = { 0x66, 77 }, [78] = { 0x6b, 78 },
	[79] = { 0x61, 79 }, [80] = { 0x62, 80 }, [81] = { 0x63, 81 }, [82] = { 0x60, 82 },
	[83] = { 0x6e, 83 }, [86] = { 0xe2, 86 }, [87] = { 0x7a, 87 }, [88] = { 0x7b, 88 },
	[96] = { 0x0d, 0x1c, true }, [97] = { 0x11, 0x1d, true }, [98] = { 0x6f, 0x35, true },
	[99] = { 0x2c, 0x37, true }, [100] = { 0x12, 0x38, true }, [102] = { 0x24, 0x47, true },
	[103] = { 0x26, 0x48, true }, [104] = { 0x21, 0x49, true }, [105] = { 0x25, 0x4b, true },
	[106] = { 0x27, 0x4d, true }, [107] = { 0x23, 0x4f, true }, [108] = { 0x28, 0x50, true },
	[109] = { 0x22, 0x51, true }, [110] = { 0x2d, 0x52, true }, [111] = { 0x2e, 0x53, true },
	[119] = { 0x13, 0x45 }, [125] = { 0x5b, 0x5b, true }, [126] = { 0x5c, 0x5c, true },
	[127] = { 0x5d, 0x5d, true },
};

static void on_real_key(int code, bool pressed)
{
	struct client *c = input_client();
	struct wb_msg m = { .type = WB_INPUT_KEY };

	if (code <= 0 || code >= 128 || !linux_keys[code].vk) {
		if (trace)
			msg("keyboard: key %d not known", code);
		return;
	}
	if (trace)
		msg("keyboard: key %d -> vk %#x scan %#x%s %s", code, linux_keys[code].vk,
		    linux_keys[code].scan, linux_keys[code].ext ? " E0" : "", pressed ? "down" : "up");
	if (!c)
		return;
	m.key.vk = (uint16_t)(linux_keys[code].vk << 8 | linux_keys[code].scan);
	m.key.pressed = pressed;
	m.key.flags = WB_KEY_SCANCODE | (linux_keys[code].ext ? WB_KEY_EXTENDED : 0);
	client_send(c, &m);
}

static void on_real_mouse_button(int button, bool pressed)
{
	static const uint32_t down[3] = { WB_MOUSE_LEFT_DOWN, WB_MOUSE_RIGHT_DOWN, WB_MOUSE_MIDDLE_DOWN };

	if (button < 0 || button > 2)
		return;
	send_mouse(pressed ? down[button] : down[button] << 1, 0);	/* *_UP follows *_DOWN */
}

/* moves of a message batch, sent once it is read (read_app()) */
static int mouse_dx, mouse_dy;

static void on_real_mouse_move(int what, int value)
{
	if (what == GAME_MOUSE_X)
		mouse_dx += value;
	else if (what == GAME_MOUSE_Y)
		mouse_dy += value;
	else if (what == GAME_MOUSE_WHEEL && value)
		send_mouse(0, value);
}

static void real_mouse_flush(void)
{
	if (!mouse_dx && !mouse_dy)
		return;
	move_cursor(cur_x + mouse_dx * cfg.mouse_speed, cur_y + mouse_dy * cfg.mouse_speed);
	mouse_dx = mouse_dy = 0;
}

/* The cursor and the wheel follow the sticks, once a tick */
static void move_tick(void)
{
	struct timespec now;
	float dt, vx = 0, vy = 0, speed;

	clock_gettime(CLOCK_MONOTONIC, &now);
	dt = (now.tv_sec - last_move.tv_sec) + (now.tv_nsec - last_move.tv_nsec) * 1e-9f;
	last_move = now;
	if (dt > 0.05f)
		dt = 0.05f;	/* after a pause */
	if (numpad.release && ms_since(&numpad.since) >= NUMPAD_MIN_MS &&
	    ms_since(&numpad.rest) >= NUMPAD_REST_MS)
		numpad_set(-1);
	if (!moving())
		return;
	speed = cfg.cursor_speed * mode.width;
	if (cfg.lstick == STICK_MOUSE) {
		vx += curve(axis[GP_AXIS_LX]);
		vy += curve(axis[GP_AXIS_LY]);
	}
	if (cfg.rstick == STICK_MOUSE) {
		vx += curve(axis[GP_AXIS_RX]) * 0.3f;	/* the fine one */
		vy += curve(axis[GP_AXIS_RY]) * 0.3f;
	}
	vx += (dpad_dir[GP_RIGHT] - dpad_dir[GP_LEFT]) * 0.25f;
	vy += (dpad_dir[GP_DOWN] - dpad_dir[GP_UP]) * 0.25f;
	if (vx || vy)
		move_cursor(cur_x + vx * speed * dt, cur_y + vy * speed * dt);
	if (cfg.rstick == STICK_TURN)
		turn_tick(dt);
	if (cfg.rstick == STICK_WHEEL || cfg.rstick == STICK_TURN) {
		int steps;

		wheel_acc -= curve(axis[GP_AXIS_RY]) * 8 * dt;	/* up = away = + */
		steps = (int)wheel_acc;
		if (steps) {
			wheel_acc -= steps;
			send_mouse(0, steps);
		}
	}
}

static void on_axis(enum gp_axis a, int value)
{
	if ((unsigned)a >= GP_AXIS_COUNT)
		return;
	if (!moving())
		clock_gettime(CLOCK_MONOTONIC, &last_move);
	axis[a] = (float)value / GAME_AXIS_MAX;
	if (cfg.rstick == STICK_ARROWS && (a == GP_AXIS_RX || a == GP_AXIS_RY)) {
		/* a direction held while the stick is out */
		static bool out[4];
		static const uint16_t arrows[4] = { 0x26, 0x28, 0x25, 0x27 };
		float v = axis[a];
		int neg = a == GP_AXIS_RX ? 2 : 0, pos = neg + 1;
		bool n = v < -0.5f, p = v > 0.5f;

		if (n != out[neg])
			send_key(arrows[neg], out[neg] = n);
		if (p != out[pos])
			send_key(arrows[pos], out[pos] = p);
	}
}

/* false once bare_app is gone */
static bool read_app(void)
{
	struct game_msg m;
	int n;

	for (;;) {
		n = game_link_recv(&m);
		if (n < 0)
			return false;
		if (n == 0) {
			numpad_update();
			real_mouse_flush();
			return true;
		}
		if (m.type == GAME_MSG_BUTTON)
			on_button(m.code, m.pressed, m.flags & GAME_BTN_STICK);
		else if (m.type == GAME_MSG_AXIS)
			on_axis(m.code, m.value);
		else if (m.type == GAME_MSG_KEY)
			on_real_key(m.value, m.pressed);
		else if (m.type == GAME_MSG_MOUSE_BUTTON)
			on_real_mouse_button(m.code, m.pressed);
		else if (m.type == GAME_MSG_MOUSE_MOVE)
			on_real_mouse_move(m.code, m.value);
	}
}

/* ---- main ------------------------------------------------------------------ */

static void set_timer(int fd, bool on)
{
	static bool armed;
	struct itimerspec it = { 0 };

	if (on == armed)
		return;
	armed = on;
	if (on)
		it.it_interval.tv_nsec = it.it_value.tv_nsec = TICK_NS;
	timerfd_settime(fd, 0, &it, NULL);
}

static void kill_wine(void)
{
	char *const argv[] = { (char *)"wineserver", (char *)"-k", NULL };

	run(argv, NULL);
}

/* not for Wine and its children: the link is closed on exec */
static int open_shm(void)
{
	if (game_link_open(&app, false) < 0) {
		msg("started by bare_app only (%s)", strerror(errno));
		return -1;
	}
	return 0;
}

static int listen_socket(char *path, size_t size)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	const char *dir = getenv("XDG_RUNTIME_DIR");
	int fd;

	snprintf(path, size, "%s/bare_app_wine.%d", dir && *dir ? dir : "/tmp", (int)getpid());
	if (strlen(path) >= sizeof(addr.sun_path))
		return -1;
	strcpy(addr.sun_path, path);
	unlink(path);
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
		msg("%s: %s", path, strerror(errno));
		return -1;
	}
	setenv(WINEBARE_SOCKET_ENV, path, 1);
	return fd;
}

int main(int argc, char **argv)
{
	char ini[PATH_MAX + 16], sock_path[108], exe[PATH_MAX * 2], *wargv[64];
	struct pollfd pfd[4 + MAX_CLIENTS];
	pid_t game = -1, waiter = -1;
	int listen_fd, sig_fd, tick_fd, status = 0, wargc = 0, i;
	sigset_t mask;

	setvbuf(stderr, NULL, _IOLBF, 0);
	trace = getenv("BARE_APP_WINE_TRACE") != NULL;
	if (argc != 2) {
		fprintf(stderr, "usage: bare_app_wine <game directory>\n");
		return 2;
	}
	if (open_shm() < 0)
		return 2;
	snprintf(cfg.dir, sizeof(cfg.dir), "%s", argv[1]);
	snprintf(ini, sizeof(ini), "%s/wine.ini", cfg.dir);
	set_defaults();
	if (read_ini(ini) < 0)
		return 1;
	set_env();
	set_mode(cfg.mode);
	cur_x = mode.width / 2;
	cur_y = mode.height / 2;
	publish();	/* black until Wine draws */

	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	sig_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	tick_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if ((listen_fd = listen_socket(sock_path, sizeof(sock_path))) < 0 || sig_fd < 0 || tick_fd < 0)
		return 1;

	/* children get the signals back (posix_spawn keeps the mask) */
	sigprocmask(SIG_UNBLOCK, &mask, NULL);
	if (prepare_prefix() < 0 || make_tree() < 0) {
		unlink(sock_path);
		return 1;
	}
	sigprocmask(SIG_BLOCK, &mask, NULL);

	/* a Unix or Windows path of its own (C:\windows\notepad.exe) as it is */
	if (cfg.exe[0] == '/' || (cfg.exe[0] && cfg.exe[1] == ':'))
		snprintf(exe, sizeof(exe), "%s", cfg.exe);
	else
		snprintf(exe, sizeof(exe), "%s/%s", cfg.dir, cfg.exe);
	wargv[wargc++] = (char *)"wine";
	wargv[wargc++] = exe;
	{
		char *save = NULL, *a;

		for (a = strtok_r(cfg.args, " ", &save); a && wargc < 63; a = strtok_r(NULL, " ", &save))
			wargv[wargc++] = a;
	}
	wargv[wargc] = NULL;
	{
		sigset_t old;

		sigemptyset(&old);
		sigprocmask(SIG_SETMASK, &old, NULL);
		game = spawn(wargv, cfg.dir, NULL);
		sigprocmask(SIG_BLOCK, &mask, NULL);
	}
	if (game < 0) {
		unlink(sock_path);
		return 1;
	}
	msg("%s started (pid %d), prefix %s", exe, (int)game, getenv("WINEPREFIX"));

	for (;;) {
		unsigned n = 0, first_client;

		set_timer(tick_fd, dirty || moving());
		pfd[n++] = (struct pollfd){ .fd = GAME_IPC_SOCK, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = listen_fd, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = sig_fd, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = tick_fd, .events = POLLIN };
		first_client = n;
		for (i = 0; i < (int)client_count; i++)
			pfd[n++] = (struct pollfd){ .fd = clients[i].fd, .events = POLLIN };
		if (poll(pfd, n, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfd[0].revents && !read_app()) {
			msg("bare_app is gone");
			break;
		}
		if (pfd[1].revents)
			accept_client(listen_fd);
		if (pfd[2].revents) {
			struct signalfd_siginfo si;
			bool quit = false;

			while (read(sig_fd, &si, sizeof(si)) == sizeof(si))
				if (si.ssi_signo != SIGCHLD)
					quit = true;
			if (quit)
				break;
			for (;;) {
				int st;
				pid_t p = waitpid(-1, &st, WNOHANG);

				if (p <= 0)
					break;
				if (p == game) {
					char *const w[] = { (char *)"wineserver", (char *)"-w", NULL };

					status = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
					msg("the game has ended (%d), waiting for Wine", status);
					game = -1;
					/* its other programs may go on: until wineserver ends */
					waiter = spawn(w, NULL, NULL);
				} else if (p == waiter) {
					unlink(sock_path);
					return status;
				}
			}
		}
		if (pfd[3].revents) {
			uint64_t ticks;

			if (read(tick_fd, &ticks, sizeof(ticks)) > 0) {
				move_tick();
				if (dirty && publish() < 0) {
					msg("bare_app is gone");
					break;
				}
			}
		}
		/* backwards: a dropped client is replaced by the last one */
		for (i = (int)(n - first_client) - 1; i >= 0; i--)
			if (pfd[first_client + i].revents && i < (int)client_count)
				while (read_client(i) && (poll(&(struct pollfd){ .fd = clients[i].fd, .events = POLLIN }, 1, 0) > 0))
					;
	}
	kill_wine();
	unlink(sock_path);
	return status;
}
