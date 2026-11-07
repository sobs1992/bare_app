/*
 * Half-Life as a video source (GAME_HALFLIFE): the Xash3D FWGS engine
 * (GPLv3) built with its bare_app platform and the game code of
 * hlsdk-portable (SDK packages xash3d-fwgs and hlsdk-portable), started per
 * session through game_proc.h. The engine renders with OpenGL ES on the GPU
 * (in software without Mesa) into the frames of the video layer; the
 * gamepad drives it as the engine's own gamepad. The game files (the valve directory of a
 * bought copy) are not included: see GAME_HALFLIFE_GAME.
 *
 * View thread only.
 */
#pragma once

#include <stdbool.h>

#include "game_proc.h"

/* Directory with the game's valve directory, read only:
 * $BARE_APP_HALFLIFE_GAME or CONFIG_GAME_HALFLIFE_GAME */
const char *halflife_game(void);

/* Settings and saved games: $BARE_APP_HALFLIFE_DATA or
 * CONFIG_GAME_HALFLIFE_DATA */
const char *halflife_data(void);

/* The first file the game needs that is missing, NULL if none */
const char *halflife_missing(void);

/*
 * NULL with errno set (ENOENT: see halflife_missing()). lang: bare_app's
 * language code; the game takes its texts from <game>/valve_<language>
 * (a bought copy has valve_russian) when there are some for it. The
 * renderer is the one chosen in the game's menu (Video modes); until one
 * is, OpenGL ES if installed and !soft, else software
 * ($BARE_APP_HALFLIFE_REF overrides all of them).
 */
struct game_proc *halflife_start(int width, int height, const char *pcm, const char *lang, bool soft);
