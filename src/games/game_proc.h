/*
 * A game in its own process as a video source: Doom (games/doom.c), Heroes
 * III (games/homm3.c). The game is a separate program, started per session,
 * and only pixels and gamepad events cross over (game_ipc.h), so no game
 * code is linked into bare_app; a crash of the game does not take the app
 * down. It draws whole frames, already scaled to the video layer, into
 * shared memory and gets the gamepad buttons and axes.
 *
 * View thread only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core/input.h"
#include "hal/hal.h"

struct game_proc;

struct game_spawn {
	const char *path;		/* program */
	char *const *argv;		/* argv[0] included, NULL-terminated */
	const char *const *env;		/* "NAME=value" added to ours, NULL-terminated; may be NULL */
	const char *dir;		/* working directory */
	int width, height;		/* frame size: the video layer */
};

/* NULL with errno set */
struct game_proc *game_proc_start(const struct game_spawn *s);
/* Readable when a frame is published or the game has ended */
int game_proc_fd(struct game_proc *g);
/* After game_proc_fd() was readable: the newest frame or NULL; *ended is
 * set once the game process is gone */
const struct hal_fb *game_proc_frame(struct game_proc *g, bool *ended);
/* Buttons (auto-repeats are not sent) and axes */
void game_proc_input(struct game_proc *g, const struct gp_event *ev);
/* Ends the game if it still runs (SIGTERM, then SIGKILL); returns its
 * waitpid() status */
int game_proc_stop(struct game_proc *g);

/* A program next to our own binary (build dir, /usr/bin); -1 if the path
 * does not fit */
int game_proc_sibling(const char *name, char *buf, size_t size);
/* dir with "~/" expanded to $HOME, created with its parents if missing */
int game_proc_mkdir(const char *dir, char *buf, size_t size);
