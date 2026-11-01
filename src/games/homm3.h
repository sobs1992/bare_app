/*
 * Heroes of Might and Magic III as a video source (GAME_HOMM3): VCMI's
 * client (SDK package vcmi, GPLv2) built with its bare_app link, started
 * per session through game_proc.h. VCMI draws in software into an SDL
 * window kept in memory (dummy video driver) of twice the video layer's
 * size and sends it scaled down 2:1, or a 1:1 part of it around the cursor;
 * the gamepad drives it as an SDL game controller.
 *
 * View thread only.
 */
#pragma once

#include "game_proc.h"

/* Game directory (Data, Maps, Mp3, Mods): $BARE_APP_HOMM3_GAME or
 * CONFIG_GAME_HOMM3_GAME */
const char *homm3_game(void);

/* Data directory: $BARE_APP_HOMM3_DATA or CONFIG_GAME_HOMM3_DATA. VCMI
 * keeps saved games in <data>/vcmi and its settings in <data>/config/vcmi. */
const char *homm3_data(void);

/* What is missing to start the game (the program or the game data), NULL
 * if nothing is */
const char *homm3_missing(void);

/* Start the game for a width x height video layer, with sound on the ALSA
 * PCM device node pcm (/dev/snd/pcmC0D0p; NULL: silent), in the language
 * lang ("en", "ru": VCMI's and its mods' texts; the original game's texts
 * are in the language of its data); NULL with errno set (ENOENT: see
 * homm3_missing()) */
struct game_proc *homm3_start(int width, int height, const char *pcm, const char *lang);
