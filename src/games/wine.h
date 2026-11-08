/*
 * Windows games under Wine (GAME_WINE): every <games>/<dir>/wine.ini is a
 * game of the main menu. A game is played by bare_app_wine (ports/wine),
 * started per session through game_proc.h, which runs Wine with the SDK's
 * winebare.drv driver and turns its windows into frames of the video
 * layer and the gamepad into mouse and keyboard (the [keys] of wine.ini).
 * Nothing of Wine is linked into bare_app.
 *
 * View thread only.
 */
#pragma once

#include "game_proc.h"

#define WINE_MAX_GAMES 16

/* Looks for the games again: how many there are (at most WINE_MAX_GAMES),
 * in the order of their names */
int wine_scan(void);
int wine_count(void);
/* The "name" of wine.ini, else the directory's name */
const char *wine_name(int i);

/* The game the next wine_start() plays and wine_missing() checks */
void wine_select(int i);
int wine_selected(void);
const char *wine_selected_name(void);

/* The first file the selected game needs that is missing, NULL if none */
const char *wine_missing(void);

/* The selected game; NULL with errno set */
struct game_proc *wine_start(int width, int height);
