/*
 * Half-Life (GAME_HALFLIFE): the Xash3D FWGS engine (GPLv3) built with its
 * bare_app platform and the game code of hlsdk-portable (SDK packages
 * xash3d-fwgs and hlsdk-portable). The engine renders with OpenGL ES on the
 * GPU (in software without Mesa) into the frames of the video layer; the
 * gamepad drives it as the engine's own gamepad. The game files (the valve
 * directory of a bought copy) are not included: see GAME_HALFLIFE_GAME.
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
#include "hal/platform.h"

#define TAG "halflife"

/*
 * What the SDK packages install under the prefix: the engine, the game code
 * of hlsdk-portable (its library names carry the architecture: hl_arm64.so)
 * and the engine's own resources (fonts, the menu's pictures).
 */
#define PROGRAM "/bin/xash3d"
#define ENGINE_LIBS "/lib/xash3d"
#define GAME_LIBS ENGINE_LIBS "/valve"
#define EXTRAS "/share/xash3d/valve/extras.pk3"
/*
 * The engine's OpenGL ES renderer, on the GPU; installed with Mesa. Mesa's
 * panfrost packs textures it sees used often into AFBC in the background:
 * on the GO Ultra's Mali G52 (r0p0, Mesa 25.0) the GPU then finds invalid
 * job descriptors after a level loads and every fault stalls the game for
 * half a second. Without AFBC it runs clean.
 */
#define GLES_REF "gles3compat"
#define GLES_ENV "PAN_MESA_DEBUG=noafbc"

static const char *env_or(const char *name, const char *def)
{
	const char *env = getenv(name);

	return env && *env ? env : def;
}

/* $BARE_APP_HALFLIFE_PREFIX: the engine installed elsewhere (tests) */
static const char *prefix(void)
{
	return env_or("BARE_APP_HALFLIFE_PREFIX", CONFIG_GAME_HALFLIFE_PREFIX);
}

/* Directory with the game's valve directory, read only */
static const char *halflife_game(void)
{
	return env_or("BARE_APP_HALFLIFE_GAME", CONFIG_GAME_HALFLIFE_GAME);
}

/* Settings and saved games */
static const char *halflife_data(void)
{
	return env_or("BARE_APP_HALFLIFE_DATA", CONFIG_GAME_HALFLIFE_DATA);
}

/*
 * <prefix>/lib/xash3d/valve/<sub>/<name>*.so into buf; -1 if there is none
 * (buf then names the file looked for)
 */
static int game_lib(const char *sub, const char *name, char *buf, size_t size)
{
	size_t len = strlen(name);
	struct dirent *e;
	int dir_len, n;
	DIR *d;

	dir_len = snprintf(buf, size, "%s" GAME_LIBS "/%s/", prefix(), sub);
	if (dir_len < 0 || (size_t)dir_len >= size)
		return -1;
	buf[dir_len - 1] = '\0';
	d = opendir(buf);
	buf[dir_len - 1] = '/';
	while (d && (e = readdir(d))) {
		size_t l = strlen(e->d_name);

		if (l > len + 3 && !strncmp(e->d_name, name, len) &&
		    (e->d_name[len] == '.' || e->d_name[len] == '_') &&
		    !strcmp(e->d_name + l - 3, ".so")) {
			n = snprintf(buf + dir_len, size - dir_len, "%s", e->d_name);
			closedir(d);
			return n >= 0 && (size_t)n < size - dir_len ? 0 : -1;
		}
	}
	if (d)
		closedir(d);
	n = snprintf(buf + dir_len, size - dir_len, "%s.so", name);
	(void)n;
	return -1;
}

static const char *halflife_missing(int entry)
{
	static char path[PATH_MAX];

	(void)entry;
	snprintf(path, sizeof(path), "%s" PROGRAM, prefix());
	if (access(path, X_OK) < 0)
		return path;
	if (game_lib("dlls", "hl", path, sizeof(path)) < 0 ||
	    game_lib("cl_dlls", "client", path, sizeof(path)) < 0)
		return path;
	snprintf(path, sizeof(path), "%s/valve/liblist.gam", halflife_game());
	if (access(path, R_OK) < 0)
		return path;
	return NULL;
}

static bool has_gpu(void)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s" ENGINE_LIBS "/libref_" GLES_REF ".so", prefix());
	return access(path, R_OK) == 0;
}

/* The renderer chosen in the engine's menu (Video modes): r_refdll, which
 * the engine keeps in <data>/valve/video.cfg ("" until one is chosen) */
static bool menu_renderer(const char *data)
{
	char path[PATH_MAX + 32], line[256];
	bool chosen = false;
	FILE *f;

	snprintf(path, sizeof(path), "%s/valve/video.cfg", data);
	f = fopen(path, "r");
	if (!f)
		return false;
	while (!chosen && fgets(line, sizeof(line), f))
		chosen = !strncmp(line, "r_refdll \"", 10) && line[10] && line[10] != '"';
	fclose(f);
	return chosen;
}

/*
 * The renderer for -ref, which outranks the engine's menu:
 * $BARE_APP_HALFLIFE_REF; else none once the menu has one chosen; else the
 * platform's: software if it asks for it or OpenGL ES is not installed.
 */
static const char *renderer(const char *data, bool soft)
{
	const char *env = getenv("BARE_APP_HALFLIFE_REF");

	if (env && *env)
		return env;
	if (menu_renderer(data))
		return NULL;
	return !soft && has_gpu() ? GLES_REF : "soft";
}

/* bare_app's language code -> the engine's language name (Steam's) */
static const char *language(const char *lang)
{
	return lang && !strncmp(lang, "ru", 2) ? "russian" : "english";
}

/*
 * The game takes its texts from <game>/valve_<language> (a bought copy has
 * valve_russian) when there are some for bare_app's language. The renderer
 * is the one chosen in the game's menu (Video modes); until one is, OpenGL
 * ES if installed and the platform does not ask for software
 * ($BARE_APP_HALFLIFE_REF overrides all of them).
 */
static struct game_proc *halflife_start(int entry, const struct game_env *ge)
{
	const char *pcm = ge->pcm, *lang = ge->lang;
	bool soft = ge->pf->halflife_soft;
	char program[PATH_MAX], server[PATH_MAX], client[PATH_MAX], data[PATH_MAX];
	char basedir[PATH_MAX + 32], rodir[PATH_MAX + 32], extras[PATH_MAX + 32], libs[PATH_MAX + 32];
	char alsa[32] = "";
	const char *missing = halflife_missing(entry);

	if (missing) {
		LOGE(TAG, "%s: %s", missing, strerror(ENOENT));
		errno = ENOENT;
		return NULL;
	}
	if (game_proc_mkdir(halflife_data(), data, sizeof(data)) < 0) {
		LOGE(TAG, "%s: %s", data, strerror(errno));
		return NULL;
	}
	snprintf(program, sizeof(program), "%s" PROGRAM, prefix());
	game_lib("dlls", "hl", server, sizeof(server));
	game_lib("cl_dlls", "client", client, sizeof(client));
	if (pcm)
		game_proc_alsa_name(pcm, alsa, sizeof(alsa));

	/*
	 * Read-only game files and a writable base directory of the engine's
	 * own, where it keeps the settings and saved games (valve/SAVE)
	 */
	snprintf(basedir, sizeof(basedir), "XASH3D_BASEDIR=%s", data);
	snprintf(rodir, sizeof(rodir), "XASH3D_RODIR=%s", halflife_game());
	snprintf(extras, sizeof(extras), "XASH3D_EXTRAS_PAK1=%s" EXTRAS, prefix());
	/* the launcher loads the engine (libxash.so) by its name only */
	snprintf(libs, sizeof(libs), "LD_LIBRARY_PATH=%s" ENGINE_LIBS, prefix());

	const char *ref = renderer(data, soft);
	char *argv[16];
	int n = 0;

	argv[n++] = (char *)"xash3d";
	if (ref) {
		argv[n++] = (char *)"-ref";
		argv[n++] = (char *)ref;
	}
	argv[n++] = (char *)"-dll";
	argv[n++] = server;
	argv[n++] = (char *)"-clientlib";
	argv[n++] = client;
	/* every start: the engine keeps the last one in vfs.cfg */
	argv[n++] = (char *)"-language";
	argv[n++] = (char *)language(lang);
	if (alsa[0]) {
		argv[n++] = (char *)"-alsadev";
		argv[n++] = alsa;
	} else {
		argv[n++] = (char *)"-nosound";
	}
	argv[n] = NULL;
	/* for the GPU renderer, whoever chose it; ignored by the software one */
	const char *const env[] = { basedir, rodir, extras, libs, GLES_ENV, NULL };
	struct game_spawn s = { .path = program, .argv = argv, .env = env, .dir = data,
				.width = ge->width, .height = ge->height };

	LOGI(TAG, "game %s, data %s, renderer %s, language %s, sound %s", halflife_game(), data,
	     ref ? ref : "of the game's menu", language(lang), alsa[0] ? alsa : "off");
	return game_proc_start(&s);
}

/* L2 and R2 are the game's */
const struct game_def game_halflife = {
	.id = "halflife",
	.title = S_HALFLIFE,
	.hint = S_HALFLIFE_HINT,
	.missing = halflife_missing,
	.start = halflife_start,
};
