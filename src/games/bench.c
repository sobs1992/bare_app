/*
 * The stress test (GAME_BENCH): bare_app_bench next to this program
 * (ports/bench). Not one of games[]: the home screen has it as a round
 * button, where Layers was. Its log goes to CONFIG_GAME_BENCH_DIR.
 */
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "game.h"

#define TAG "bench"
#define GAME "bare_app_bench"

static const char *bench_missing(int entry)
{
	static char path[PATH_MAX];

	(void)entry;
	if (game_proc_sibling(GAME, path, sizeof(path)) < 0)
		return GAME;
	return access(path, X_OK) == 0 ? NULL : path;
}

static struct game_proc *bench_start(int entry, const struct game_env *ge)
{
	char game[PATH_MAX], dir[PATH_MAX];
	char *const argv[] = { (char *)GAME, (char *)"-lang", (char *)(ge->lang ? ge->lang : "en"),
			       (char *)"-o", (char *)"bench.log", NULL };
	struct game_spawn s = { .path = game, .argv = argv, .dir = dir,
				.width = ge->width, .height = ge->height };

	(void)entry;
	if (game_proc_sibling(GAME, game, sizeof(game)) < 0 || access(game, X_OK) < 0) {
		LOGE(TAG, "no %s next to this program", GAME);
		errno = ENOENT;
		return NULL;
	}
	if (game_proc_mkdir(CONFIG_GAME_BENCH_DIR, dir, sizeof(dir)) < 0) {
		LOGE(TAG, "%s: %s", dir, strerror(errno));
		return NULL;
	}
	LOGI(TAG, "log %s/bench.log", dir);
	return game_proc_start(&s);
}

const struct game_def game_bench = {
	.id = "bench",
	.title = S_BENCH,
	.hint = S_BENCH_HINT,
	.missing = bench_missing,
	.start = bench_start,
};
