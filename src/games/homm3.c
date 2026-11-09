/*
 * Heroes of Might and Magic III (GAME_HOMM3): VCMI's client (SDK package
 * vcmi, GPLv2) built with its bare_app link. VCMI draws in software into
 * an SDL window kept in memory (dummy video driver) and sends it fitted to
 * the frame as the platform's "homm3.view" says, or a 1:1 part of it around
 * the cursor; the gamepad drives it as an SDL game controller, with the F
 * keys "homm3.keys" names (platform game_opts).
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

/*
 * homm3.view, how VCMI fills the frame:
 *   double  (default) drawn at twice the frame, taken 2:1 or as a 1:1
 *           magnifier (L1)
 *   native  1:1: VCMI's screen the frame's size, at least the 800x600 its
 *           windows need; the frame is the part the cursor points at
 *   fit     the same screen whole in the frame, each axis by itself
 *           (854x480: 854x600 squashed down only), L1 a 1:1 magnifier
 * homm3.keys, Heroes III's F keys:
 *   oga     (default) F1, F2 held: Ctrl, Shift; F3 cancel, F4 options,
 *           F5 minimise
 *   ogu     F1 minimise, F2, F3 held: Ctrl, Shift; F4 accept, F5 cancel or
 *           end the turn; the right stick as the D-pad
 */
static bool opt_is(const struct platform *pf, const char *key, const char *value)
{
	const char *v = platform_opt(pf, key);

	return v && !strcmp(v, value);
}

/* VCMI looks for the game in <dir>/vcmi of each $XDG_DATA_DIRS entry: this
 * directory gets the link vcmi -> the game directory */
#define LINK_DIR "/tmp/bare_app_homm3"

/* Game directory (Data, Maps, Mp3, Mods) */
static const char *homm3_game(void)
{
	return game_setting("BARE_APP_HOMM3_GAME", CONFIG_GAME_HOMM3_GAME);
}

/* Saved games in <data>/vcmi, settings in <data>/config/vcmi */
static const char *homm3_data(void)
{
	return game_setting("BARE_APP_HOMM3_DATA", CONFIG_GAME_HOMM3_DATA);
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
	char *const argv[] = { (char *)"vcmiclient", (char *)"--nointro", NULL };
	const char *missing = homm3_missing(entry);
	struct game_vars env = { 0 };
	char alsa[32];

	if (missing) {
		LOGE(TAG, "%s: %s", missing, strerror(ENOENT));
		errno = ENOENT;
		return NULL;
	}
	if (game_link() < 0) {
		LOGE(TAG, "%s/vcmi: %s", LINK_DIR, strerror(errno));
		return NULL;
	}
	game_vars_add(&env, "SDL_VIDEODRIVER=dummy");
	/* SIGTERM ends the game; SDL would turn it into "quit?" */
	game_vars_add(&env, "SDL_NO_SIGNAL_HANDLERS=1");
	game_vars_add(&env, "XDG_DATA_DIRS=" LINK_DIR);
	game_vars_add(&env, "XDG_DATA_HOME=%s", homm3_data());
	game_vars_add(&env, "XDG_CONFIG_HOME=%s/config", homm3_data());
	game_vars_add(&env, "XDG_CACHE_HOME=%s/cache", homm3_data());
	/* VCMI's link turns it into its own language setting */
	game_vars_add(&env, "BARE_APP_LANG=%s", ge->lang);
	game_vars_sdl_audio(&env, ge->pcm, alsa, sizeof(alsa));
	if (opt_is(ge->pf, "homm3.view", "native"))
		game_vars_add(&env, "BARE_APP_VCMI_NATIVE=1");
	else if (opt_is(ge->pf, "homm3.view", "fit"))
		game_vars_add(&env, "BARE_APP_VCMI_FIT=1");
	if (opt_is(ge->pf, "homm3.keys", "ogu"))
		game_vars_add(&env, "BARE_APP_VCMI_KEYS=ogu");
	struct game_spawn s = { .path = CONFIG_GAME_HOMM3_PROGRAM, .argv = argv, .env = env.v,
				.dir = homm3_data(), .width = ge->width, .height = ge->height,
				/* a USB keyboard and mouse as they are (VCMI patch 0012) */
				.keyboard = true, .mouse = true };

	LOGI(TAG, "game %s, data %s, sound %s, language %s", homm3_game(), homm3_data(),
	     alsa[0] ? alsa : "off", ge->lang);
	return game_proc_start(&s);
}

static enum str_id homm3_hint(const struct platform *pf)
{
	return opt_is(pf, "homm3.keys", "ogu") ? S_HOMM3_HINT_OGU : S_HOMM3_HINT;
}

/* F5 is the game's on the GO Ultra: cancel or end the turn */
static enum gp_button homm3_minimize(const struct platform *pf)
{
	return opt_is(pf, "homm3.keys", "ogu") ? GP_F1 : GP_F5;
}

/* <game directory>/cover.png */
static void homm3_info(int entry, struct game_info *gi)
{
	static char cover[PATH_MAX];

	(void)entry;
	gi->cover = game_cover_in(cover, sizeof(cover), homm3_game());
}

const struct game_def game_homm3 = {
	.id = "homm3",
	.title = S_HOMM3,
	.hint = S_HOMM3_HINT,
	.trigger_volume = true,
	.info = homm3_info,
	.missing = homm3_missing,
	.start = homm3_start,
	.hint_for = homm3_hint,
	.minimize_for = homm3_minimize,
};
