/* The games of this build (game.h): one line each, in menu order */
#include "game.h"

#include <stdio.h>
#include <unistd.h>

extern const struct game_def game_homm3, game_halflife, game_doom, game_native, game_wine;

const struct game_def *const games[] = {
#ifdef CONFIG_GAME_HOMM3
	&game_homm3,
#endif
#ifdef CONFIG_GAME_HALFLIFE
	&game_halflife,
#endif
#ifdef CONFIG_GAME_DOOM
	&game_doom,
#endif
#ifdef CONFIG_GAME_NATIVE
	&game_native,
#endif
#ifdef CONFIG_GAME_WINE
	&game_wine,
#endif
	NULL
};

int game_entries(const struct game_def *g, bool rescan)
{
	return g->entries ? g->entries(rescan) : 1;
}

static struct game_info info(struct game_ref g)
{
	struct game_info gi = { .trigger_volume = g.def->trigger_volume };

	if (g.def->info)
		g.def->info(g.entry, &gi);
	return gi;
}

const char *game_title(struct game_ref g)
{
	const char *name;

	if (!g.def)
		return "";
	name = info(g).name;
	return name ? name : tr(g.def->title);
}

const char *game_hint(struct game_ref g, const struct platform *pf)
{
	const char *hint = info(g).hint;

	if (hint)
		return hint;
	return tr(g.def->hint_for ? g.def->hint_for(pf) : g.def->hint);
}

const char *game_cover(struct game_ref g)
{
	const char *cover = g.def ? info(g).cover : NULL;

	return cover && *cover && access(cover, R_OK) == 0 ? cover : NULL;
}

const char *game_cover_in(char *buf, size_t size, const char *dir)
{
	if (!dir || !*dir || snprintf(buf, size, "%s/cover.png", dir) >= (int)size)
		return NULL;
	return buf;
}

bool game_trigger_volume(struct game_ref g)
{
	return g.def && info(g).trigger_volume;
}

enum gp_button game_minimize_button(struct game_ref g, const struct platform *pf)
{
	return g.def && g.def->minimize_for ? g.def->minimize_for(pf) : GP_F5;
}

bool game_missing_text(struct game_ref g, char *buf, size_t size)
{
	const char *missing = g.def ? g.def->missing(g.entry) : NULL;

	if (!missing)
		return false;
	snprintf(buf, size, tr(g.def->missing_fmt ? g.def->missing_fmt : S_GAME_MISSING_FMT), missing);
	return true;
}
