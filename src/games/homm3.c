/*
 * Heroes of Might and Magic III (GAME_HOMM3): VCMI's client (SDK package
 * vcmi, GPLv2) built with its bare_app link. VCMI draws in software into
 * an SDL window kept in memory (dummy video driver) and sends it fitted to
 * the frame as the platform's homm3_view says, or a 1:1 part of it around
 * the cursor; the gamepad drives it as an SDL game controller, with the F
 * keys of the platform's homm3_keys.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/log.h"
#include "game.h"
#include "hal/platform.h"

#define TAG "homm3"

/* VCMI looks for the game in <dir>/vcmi of each $XDG_DATA_DIRS entry: this
 * directory gets the link vcmi -> the game directory */
#define LINK_DIR "/tmp/bare_app_homm3"

/* Game directory (Data, Maps, Mp3, Mods) */
static const char *homm3_game(void)
{
	const char *env = getenv("BARE_APP_HOMM3_GAME");

	return env && *env ? env : CONFIG_GAME_HOMM3_GAME;
}

/* Saved games in <data>/vcmi, settings in <data>/config/vcmi */
static const char *homm3_data(void)
{
	const char *env = getenv("BARE_APP_HOMM3_DATA");

	return env && *env ? env : CONFIG_GAME_HOMM3_DATA;
}

static const char *homm3_missing(int entry)
{
	static char path[PATH_MAX];

	(void)entry;
	if (access(CONFIG_GAME_HOMM3_PROGRAM, X_OK) < 0)
		return CONFIG_GAME_HOMM3_PROGRAM;
	snprintf(path, sizeof(path), "%s/Data", homm3_game());
	if (access(path, R_OK) < 0)
		return path;
	return NULL;
}

static int game_link(void)
{
	if (mkdir(LINK_DIR, 0755) < 0 && errno != EEXIST)
		return -1;
	unlink(LINK_DIR "/vcmi");
	return symlink(homm3_game(), LINK_DIR "/vcmi");
}

/* The language: VCMI's and its mods' texts; the original game's are in
 * the language of its data */
static struct game_proc *homm3_start(int entry, const struct game_env *ge)
{
	const char *pcm = ge->pcm, *lang = ge->lang;
	enum homm3_view view = ge->pf->homm3_view;
	enum homm3_keys keys = ge->pf->homm3_keys;
	char xdg_data[PATH_MAX + 16], xdg_config[PATH_MAX + 32], xdg_cache[PATH_MAX + 32];
	char audiodev[64] = "", name[32], language[32];
	char *const argv[] = { (char *)"vcmiclient", (char *)"--nointro", NULL };
	const char *missing = homm3_missing(entry);

	if (missing) {
		LOGE(TAG, "%s: %s", missing, strerror(ENOENT));
		errno = ENOENT;
		return NULL;
	}
	if (game_link() < 0) {
		LOGE(TAG, "%s/vcmi: %s", LINK_DIR, strerror(errno));
		return NULL;
	}
	snprintf(xdg_data, sizeof(xdg_data), "XDG_DATA_HOME=%s", homm3_data());
	snprintf(xdg_config, sizeof(xdg_config), "XDG_CONFIG_HOME=%s/config", homm3_data());
	snprintf(xdg_cache, sizeof(xdg_cache), "XDG_CACHE_HOME=%s/cache", homm3_data());
	if (pcm && game_proc_alsa_name(pcm, name, sizeof(name)) == 0)
		snprintf(audiodev, sizeof(audiodev), "AUDIODEV=%s", name);
	/* VCMI's link turns it into its own language setting */
	snprintf(language, sizeof(language), "BARE_APP_LANG=%s", lang);

	const char *env[16] = {
		"SDL_VIDEODRIVER=dummy",
		/* SIGTERM ends the game; SDL would turn it into "quit?" */
		"SDL_NO_SIGNAL_HANDLERS=1",
		"XDG_DATA_DIRS=" LINK_DIR, xdg_data, xdg_config, xdg_cache, language,
		audiodev[0] ? "SDL_AUDIODRIVER=alsa" : "SDL_AUDIODRIVER=dummy",
	};
	size_t n = 0;

	while (env[n])
		n++;
	if (audiodev[0])
		env[n++] = audiodev;
	if (view == HOMM3_VIEW_NATIVE)
		env[n++] = "BARE_APP_VCMI_NATIVE=1";
	else if (view == HOMM3_VIEW_FIT)
		env[n++] = "BARE_APP_VCMI_FIT=1";
	if (keys == HOMM3_KEYS_OGU)
		env[n++] = "BARE_APP_VCMI_KEYS=ogu";
	env[n] = NULL;
	struct game_spawn s = { .path = CONFIG_GAME_HOMM3_PROGRAM, .argv = argv, .env = env,
				.dir = homm3_data(), .width = ge->width, .height = ge->height };

	LOGI(TAG, "game %s, data %s, sound %s, language %s", homm3_game(), homm3_data(),
	     audiodev[0] ? name : "off", lang);
	return game_proc_start(&s);
}

static enum str_id homm3_hint(const struct platform *pf)
{
	return pf->homm3_keys == HOMM3_KEYS_OGU ? S_HOMM3_HINT_OGU : S_HOMM3_HINT;
}

/* F5 is the game's on the GO Ultra: cancel or end the turn */
static enum gp_button homm3_minimize(const struct platform *pf)
{
	return pf->homm3_keys == HOMM3_KEYS_OGU ? GP_F1 : GP_F5;
}

const struct game_def game_homm3 = {
	.id = "homm3",
	.title = S_HOMM3,
	.hint = S_HOMM3_HINT,
	.trigger_volume = true,
	.missing = homm3_missing,
	.start = homm3_start,
	.hint_for = homm3_hint,
	.minimize_for = homm3_minimize,
};
