/*
 * Doom as a video source (GAME_DOOM): bare_app_doom next to this program
 * (ports/doom: doomgeneric, GPLv2), started per session through
 * game_proc.h. It keeps its state in globals and leaves with exit(), so a
 * process is also the only way to restart the engine.
 *
 * View thread only.
 */
#pragma once

#include "game_proc.h"

/* IWAD: $BARE_APP_DOOM_WAD, else the first known one in
 * CONFIG_GAME_DOOM_GAME, else CONFIG_GAME_DOOM_WAD */
const char *doom_wad(void);

/* Start the game for a width x height video layer, with sound on the ALSA
 * PCM device pcm (NULL: silent); NULL with errno set (ENOENT: no IWAD or
 * no bare_app_doom) */
struct game_proc *doom_start(int width, int height, const char *pcm);
