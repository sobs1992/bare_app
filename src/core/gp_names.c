/*
 * The names of the gamepad buttons (enum gp_button) as the settings file
 * and the games' ini files write them. The one table: bare_app, bare_app_wine
 * and the tests compile this file.
 */
#include "input.h"

#include <strings.h>

static const char *const names[GP_COUNT] = {
	[GP_UP] = "up", [GP_DOWN] = "down", [GP_LEFT] = "left", [GP_RIGHT] = "right",
	[GP_A] = "a", [GP_B] = "b", [GP_X] = "x", [GP_Y] = "y",
	[GP_L1] = "l1", [GP_R1] = "r1", [GP_L2] = "l2", [GP_R2] = "r2",
	[GP_SELECT] = "select", [GP_START] = "start", [GP_MENU] = "menu",
	[GP_F1] = "f1", [GP_F2] = "f2", [GP_F3] = "f3",
	[GP_F4] = "f4", [GP_F5] = "f5", [GP_F6] = "f6",
	[GP_VOL_UP] = "vol+", [GP_VOL_DOWN] = "vol-", [GP_POWER] = "power",
};

const char *gp_button_name(enum gp_button b)
{
	return b >= 0 && b < GP_COUNT ? names[b] : "?";
}

enum gp_button gp_button_from_name(const char *name)
{
	int b;

	for (b = 0; b < GP_COUNT; b++)
		if (!strcasecmp(names[b], name))
			return b;
	return GP_NONE;
}
