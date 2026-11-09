/*
 * The games of bare_app. Each one is a program of its own, started per
 * session through game_proc.h, with only frames and gamepad events
 * crossing over (game_ipc.h). A game is one struct game_def in its own
 * src/games/<game>.c, listed in games.c: the main menu, the game screen
 * and the view core know the games only through that table.
 *
 * Adding a game: src/games/<game>.c with its game_def, a line in games.c,
 * a GAME_<GAME> symbol in Kconfig (select GAMES), add_if() in
 * CMakeLists.txt, its strings in i18n.
 *
 * View thread only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core/input.h"
#include "game_proc.h"
#include "gui/view/core/i18n.h"

struct platform;

/* What a game is started with */
struct game_env {
	const struct platform *pf;
	int width, height;	/* the video layer: the frames' size */
	const char *pcm;	/* ALSA PCM device node games play to; NULL: silent */
	const char *lang;	/* bare_app's language code ("en", "ru") */
};

/* What an entry found at run time says of itself (game_def.info) */
struct game_info {
	const char *name;	/* NULL: the game's title */
	const char *hint;	/* NULL: the game's hint */
	const char *cover;	/* picture of it for the home screen (PNG), NULL none */
	bool trigger_volume;	/* as game_def.trigger_volume */
};

struct game_def {
	const char *id;		/* for the log */
	enum str_id title;	/* menu entry and screen title, unless info() names it */
	enum str_id hint;	/* the controls, shown once the first frame is in */
	/* "%s" text that tells what missing() returned; 0: S_GAME_MISSING_FMT */
	enum str_id missing_fmt;
	bool trigger_volume;	/* L2, R2: the volume instead of the game */
	/*
	 * Games found at run time (Windows games of the games partition):
	 * how many entries there are, looked for again if rescan. NULL: one.
	 * Entry numbers stay valid until the next rescan.
	 */
	int (*entries)(bool rescan);
	/* The entry's own name, hint, cover, triggers (starts from the game's); may be NULL */
	void (*info)(int entry, struct game_info *gi);
	/* The first thing the entry needs that is missing (a path), NULL if none */
	const char *(*missing)(int entry);
	/* NULL with errno set (ENOENT: see missing()) */
	struct game_proc *(*start)(int entry, const struct game_env *env);
	/* Optional, by platform: another hint, another minimise button (F5) */
	enum str_id (*hint_for)(const struct platform *pf);
	enum gp_button (*minimize_for)(const struct platform *pf);
};

/* The games of this build in menu order, NULL-terminated */
extern const struct game_def *const games[];

/* One entry of a game; def NULL: no game */
struct game_ref {
	const struct game_def *def;
	int entry;
};

static inline bool game_ref_eq(struct game_ref a, struct game_ref b)
{
	return a.def == b.def && (!a.def || a.entry == b.entry);
}

/* Menu entries of g (see game_def.entries) */
int game_entries(const struct game_def *g, bool rescan);
/* Its title and hint in the current language, whether L2 and R2 are the volume */
const char *game_title(struct game_ref g);
const char *game_hint(struct game_ref g, const struct platform *pf);
bool game_trigger_volume(struct game_ref g);
/* A name of the entry that stays the same between runs and builds: 8 hex
 * digits of its id and, for a game found at run time, its name */
#define GAME_KEY_SIZE 9
const char *game_key(struct game_ref g, char buf[GAME_KEY_SIZE]);
/* Its cover picture if the file is there, else NULL */
const char *game_cover(struct game_ref g);
/* "<dir>/cover.png" into buf (dir may be "" for none: NULL) */
const char *game_cover_in(char *buf, size_t size, const char *dir);
enum gp_button game_minimize_button(struct game_ref g, const struct platform *pf);
/* Why it cannot start into buf; false if nothing is missing */
bool game_missing_text(struct game_ref g, char *buf, size_t size);
