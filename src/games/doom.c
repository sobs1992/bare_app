#include "doom.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"

#define TAG "doom"
#define GAME "bare_app_doom"

/* IWADs looked for in CONFIG_GAME_DOOM_GAME, the full games first */
static const char *const iwads[] = {
	"doom2.wad", "plutonia.wad", "tnt.wad", "doom.wad",
	"freedoom2.wad", "freedoom1.wad", "doom1.wad",
};

const char *doom_wad(void)
{
	static char path[PATH_MAX];
	const char *env = getenv("BARE_APP_DOOM_WAD");

	if (env && *env)
		return env;
	for (size_t i = 0; CONFIG_GAME_DOOM_GAME[0] && i < sizeof(iwads) / sizeof(iwads[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", CONFIG_GAME_DOOM_GAME, iwads[i]);
		if (access(path, R_OK) == 0)
			return path;
	}
	return CONFIG_GAME_DOOM_WAD;
}

struct game_proc *doom_start(int width, int height, const char *pcm)
{
	char game[PATH_MAX], dir[PATH_MAX];
	char *const argv[] = { (char *)GAME, (char *)"-iwad", (char *)doom_wad(),
			       (char *)(pcm ? "-pcm" : "-nosound"), (char *)pcm, NULL };
	struct game_spawn s = { .path = game, .argv = argv, .dir = dir,
				.width = width, .height = height };

	if (access(doom_wad(), R_OK) < 0) {
		LOGE(TAG, "IWAD %s: %s", doom_wad(), strerror(errno));
		errno = ENOENT;
		return NULL;
	}
	if (game_proc_sibling(GAME, game, sizeof(game)) < 0 || access(game, X_OK) < 0) {
		LOGE(TAG, "no %s next to this program", GAME);
		errno = ENOENT;
		return NULL;
	}
	/* config and saves go to "." */
	if (game_proc_mkdir(CONFIG_GAME_DOOM_DIR, dir, sizeof(dir)) < 0) {
		LOGE(TAG, "%s: %s", dir, strerror(errno));
		return NULL;
	}
	LOGI(TAG, "-iwad %s, sound %s", doom_wad(), pcm ? pcm : "off");
	return game_proc_start(&s);
}
