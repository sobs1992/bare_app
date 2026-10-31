/*
 * Doom as a video source (GAME_DOOM). The game is a separate program,
 * bare_app_doom next to this one (ports/doom: doomgeneric, GPLv2), started
 * per session. Only pixels and button codes cross over (doom_ipc.h), so no
 * GPL code is linked into bare_app. A process is also the only way to
 * restart the engine and to survive its crashes:
 * it keeps its state in globals and leaves with exit().
 * It draws whole frames, already scaled to the video layer, into shared
 * memory and gets the gamepad buttons (see doom_ipc.h).
 *
 * View thread only.
 */
#pragma once

#include <stdbool.h>

#include "core/input.h"
#include "hal/hal.h"

struct doom;

/* IWAD: $BARE_APP_DOOM_WAD or CONFIG_GAME_DOOM_WAD */
const char *doom_wad(void);

/* Start the game for a width x height video layer, with sound on the ALSA
 * PCM device pcm (NULL: silent); NULL with errno set (ENOENT: no IWAD or
 * no bare_app_doom) */
struct doom *doom_start(int width, int height, const char *pcm);
/* Readable when a frame is published or the game has ended */
int doom_fd(struct doom *d);
/* After doom_fd() was readable: the newest frame or NULL; *ended is set
 * once the game process is gone */
const struct hal_fb *doom_frame(struct doom *d, bool *ended);
void doom_input(struct doom *d, const struct gp_event *ev);
/* Ends the game if it still runs; returns its waitpid() status */
int doom_stop(struct doom *d);
