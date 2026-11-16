/*
 * What a gamepad control does in a Windows game when its wine.ini [keys]
 * says nothing: the one list for bare_app_wine (set_defaults(), the game's
 * side) and bare_app's controls screen (games/wine.c, what it shows).
 * Actions as [keys] writes them (ports/wine/wine.ini).
 */
#pragma once

#include <stddef.h>
#include <string.h>

static const struct { const char *control, *action; } wine_default_keys[] = {
	{ "up", "up" }, { "down", "down" }, { "left", "left" }, { "right", "right" },
	{ "a", "lbutton" }, { "b", "rbutton" }, { "x", "enter" }, { "y", "space" },
	{ "l1", "pgup" }, { "r1", "pgdn" }, { "l2", "shift" }, { "r2", "ctrl" },
	{ "select", "tab" }, { "start", "esc" },
	{ "f1", "f1" }, { "f2", "f2" }, { "f3", "f3" }, { "f4", "f4" },
	{ "stick", "mouse" }, { "rstick", "wheel" },
};
#define WINE_DEFAULT_KEYS (sizeof(wine_default_keys) / sizeof(wine_default_keys[0]))

/* The left (or right) stick's default use ("mouse", "wheel") */
static inline const char *wine_default_stick(int left)
{
	size_t i;

	for (i = 0; i < WINE_DEFAULT_KEYS; i++)
		if (!strcmp(wine_default_keys[i].control, left ? "stick" : "rstick"))
			return wine_default_keys[i].action;
	return "none";
}
