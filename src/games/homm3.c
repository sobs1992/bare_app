#include "homm3.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/log.h"

#define TAG "homm3"

/* VCMI looks for the game in <dir>/vcmi of each $XDG_DATA_DIRS entry: this
 * directory gets the link vcmi -> the game directory */
#define LINK_DIR "/tmp/bare_app_homm3"

const char *homm3_game(void)
{
	const char *env = getenv("BARE_APP_HOMM3_GAME");

	return env && *env ? env : CONFIG_GAME_HOMM3_GAME;
}

const char *homm3_data(void)
{
	const char *env = getenv("BARE_APP_HOMM3_DATA");

	return env && *env ? env : CONFIG_GAME_HOMM3_DATA;
}

const char *homm3_missing(void)
{
	static char path[PATH_MAX];

	if (access(CONFIG_GAME_HOMM3_PROGRAM, X_OK) < 0)
		return CONFIG_GAME_HOMM3_PROGRAM;
	snprintf(path, sizeof(path), "%s/Data", homm3_game());
	if (access(path, R_OK) < 0)
		return path;
	return NULL;
}

/* /dev/snd/pcmC<card>D<dev>p -> plughw:<card>,<dev> for alsa-lib (SDL) */
static int alsa_name(const char *node, char *buf, size_t size)
{
	unsigned card, dev;

	if (sscanf(node, "/dev/snd/pcmC%uD%up", &card, &dev) != 2)
		return -1;
	snprintf(buf, size, "plughw:%u,%u", card, dev);
	return 0;
}

static int game_link(void)
{
	if (mkdir(LINK_DIR, 0755) < 0 && errno != EEXIST)
		return -1;
	unlink(LINK_DIR "/vcmi");
	return symlink(homm3_game(), LINK_DIR "/vcmi");
}

struct game_proc *homm3_start(int width, int height, const char *pcm, const char *lang, bool native)
{
	char xdg_data[PATH_MAX + 16], xdg_config[PATH_MAX + 32], xdg_cache[PATH_MAX + 32];
	char audiodev[64] = "", name[32], language[32];
	char *const argv[] = { (char *)"vcmiclient", (char *)"--nointro", NULL };
	const char *missing = homm3_missing();

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
	if (pcm && alsa_name(pcm, name, sizeof(name)) == 0)
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
	if (native)
		env[n++] = "BARE_APP_VCMI_NATIVE=1";
	env[n] = NULL;
	struct game_spawn s = { .path = CONFIG_GAME_HOMM3_PROGRAM, .argv = argv, .env = env,
				.dir = homm3_data(), .width = width, .height = height };

	LOGI(TAG, "game %s, data %s, sound %s, language %s", homm3_game(), homm3_data(),
	     audiodev[0] ? name : "off", lang);
	return game_proc_start(&s);
}
