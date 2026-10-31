/*
 * SDL2 window and keyboard for the PC simulator. Both live in the view
 * thread; the input driver pumps SDL events for the window too.
 *
 * Keys: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q/W = L1/R1,
 * Enter = Start, Backspace = Select, 1..6 = F1..F6, Esc = B.
 * Closing the window quits like SIGTERM.
 */
#pragma once

#include "hal/hal.h"

struct sdl_params {
	int width, height;	/* logical size */
	int zoom;		/* window scale */
};

extern const struct hal_display_ops display_sdl;
extern const struct hal_input_ops input_sdl;
