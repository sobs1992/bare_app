/*
 * Text console handling for displays that take over the screen: switch
 * the VT to KD_GRAPHICS so fbcon stops drawing (and blinking the cursor),
 * and switch back on exit, also after a crash.
 */
#pragma once

void console_graphics(const char *tty);	/* NULL: do nothing */
void console_restore(void);

/* Current fbcon rotation (fbcon=rotate:N), 0 if unknown */
int console_rotation(void);
