/* The games of this build (game.h): one line each, in menu order */
#include "game.h"

#include <stdint.h>
#include <strings.h>
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
	struct game_info gi = { .trigger_volume = g.def->trigger_volume, .minimize = -1,
				.gyro_on = g.def->gyro_on, .gyro_counts = g.def->gyro_counts };

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

const char *game_key(struct game_ref g, char buf[GAME_KEY_SIZE])
{
	const char *name = g.def && g.def->entries ? info(g).name : NULL;
	uint32_t h = 2166136261u;	/* FNV-1a */
	const char *p;

	for (p = g.def ? g.def->id : ""; *p; p++)
		h = (h ^ (uint8_t)*p) * 16777619u;
	if (name) {
		h = (h ^ '/') * 16777619u;
		for (p = name; *p; p++)
			h = (h ^ (uint8_t)*p) * 16777619u;
	}
	snprintf(buf, GAME_KEY_SIZE, "%08x", (unsigned)h);
	return buf;
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

/* The names of the controls in the games' ini files (wine.ini [keys]
 * and [labels], game.ini [labels]): bare_app_wine's */
static const char *const control_names[GAME_CONTROLS] = {
	[GP_UP] = "up", [GP_DOWN] = "down", [GP_LEFT] = "left", [GP_RIGHT] = "right",
	[GP_A] = "a", [GP_B] = "b", [GP_X] = "x", [GP_Y] = "y",
	[GP_L1] = "l1", [GP_R1] = "r1", [GP_L2] = "l2", [GP_R2] = "r2",
	[GP_SELECT] = "select", [GP_START] = "start", [GP_MENU] = "menu",
	[GP_F1] = "f1", [GP_F2] = "f2", [GP_F3] = "f3", [GP_F4] = "f4", [GP_F5] = "f5", [GP_F6] = "f6",
	[GAME_STICK_LEFT] = "stick", [GAME_STICK_RIGHT] = "rstick",
};

int game_control_by_name(const char *name)
{
	int c;

	for (c = 0; c < GAME_CONTROLS; c++)
		if (control_names[c] && !strcasecmp(control_names[c], name))
			return c;
	return -1;
}

const char *game_control_label(struct game_ref g, int c)
{
	const char *l;

	if (!g.def || !g.def->label || c < 0 || c >= GAME_CONTROLS)
		return NULL;
	l = g.def->label(g.entry, c);
	return l && *l ? l : NULL;
}

bool game_trigger_volume(struct game_ref g)
{
	return g.def && info(g).trigger_volume;
}

enum gp_button game_minimize_button(struct game_ref g, const struct platform *pf)
{
	int own = g.def ? info(g).minimize : -1;

	if (own >= 0)
		return own;
	return g.def && g.def->minimize_for ? g.def->minimize_for(pf) : GP_F5;
}

bool game_gyro(struct game_ref g, float *counts)
{
	struct game_info gi;

	if (!g.def) {
		*counts = GAME_GYRO_COUNTS;
		return false;
	}
	gi = info(g);
	*counts = gi.gyro_counts > 0 ? gi.gyro_counts : GAME_GYRO_COUNTS;
	return gi.gyro_on;
}

bool game_missing_text(struct game_ref g, char *buf, size_t size)
{
	const char *missing = g.def ? g.def->missing(g.entry) : NULL;

	if (!missing)
		return false;
	snprintf(buf, size, tr(g.def->missing_fmt ? g.def->missing_fmt : S_GAME_MISSING_FMT), missing);
	return true;
}
