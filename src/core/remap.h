/*
 * A game's own controls: which gamepad button each button of the device
 * is for that game. The view core applies it to what goes to the game
 * (screens/game.c); the buttons bare_app keeps for itself (the minimise
 * button, the quit combo, the volume) are taken before it.
 *
 * Each axis of the sticks can be inverted for the game as well: the game
 * gets the axis the other way round, and the left stick's directions (the
 * D-pad it makes) swap with it.
 *
 * Kept as text in the config: "a=b,b=a,x=-,ly=invert" (gp_button_name()s,
 * "-" for a button that does nothing in the game; lx, ly, rx, ry of an
 * inverted axis), only what differs.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/input.h"

struct gp_remap {
	int8_t to[GP_COUNT];	/* enum gp_button, GP_NONE: nothing */
	uint8_t invert;		/* GP_BIT(enum gp_axis) of the inverted axes */
};

void gp_remap_reset(struct gp_remap *m);		/* every button itself, no axis inverted */
bool gp_remap_is_default(const struct gp_remap *m);
/* What button b is for the game (GP_NONE: nothing) */
static inline enum gp_button gp_remap_apply(const struct gp_remap *m, enum gp_button b)
{
	return b >= 0 && b < GP_COUNT ? (enum gp_button)m->to[b] : b;
}
/* An axis' value for the game */
static inline float gp_remap_axis(const struct gp_remap *m, enum gp_axis a, float value)
{
	return a >= 0 && a < GP_AXIS_COUNT && (m->invert & GP_BIT(a)) ? -value : value;
}
/* A direction the left stick made (gp_event.stick), for the game */
enum gp_button gp_remap_stick_dir(const struct gp_remap *m, enum gp_button b);
/* lx, ly, rx, ry; NULL for none */
const char *gp_axis_name(enum gp_axis a);
/* From the config text; pairs it does not know are skipped. -1 if any was */
int gp_remap_parse(struct gp_remap *m, const char *text);
/* To it: "" for the default; false if size was too small */
bool gp_remap_format(const struct gp_remap *m, char *buf, size_t size);
