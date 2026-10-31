/*
 * Host PC.
 *
 *   pc      SDL window and keyboard when built with SDL2, otherwise the
 *           display in memory; input script from stdin in both cases
 *   pc-mem  always the display in memory: tests and scripted screenshots
 *           without a window (BARE_APP_SHOT=file.ppm)
 */
#include <stddef.h>

#include "drivers/display_mem.h"
#include "drivers/input_script.h"
#include "hal/platform.h"

#ifdef BARE_APP_WITH_SDL
#include "drivers/sdl.h"
#endif

static const struct mem_display_params mem_params = { .width = 480, .height = 320 };
static const struct script_params script_params = { .interval_ms = 150 };

const struct platform platform_pc = {
	.name = "pc",
	.compatible = NULL,
#ifdef BARE_APP_WITH_SDL
	.display = { &display_sdl, &(const struct sdl_params){ .width = 480, .height = 320, .zoom = 2 } },
#else
	.display = { &display_mem, &mem_params },
#endif
	.inputs = {
#ifdef BARE_APP_WITH_SDL
		{ &input_sdl, NULL },
#endif
		{ &input_script, &script_params },
	},
	.quit_combo = GP_BIT(GP_F6),
};

const struct platform platform_pc_mem = {
	.name = "pc-mem",
	.compatible = NULL,
	.display = { &display_mem, &mem_params },
	.inputs = {
		{ &input_script, &script_params },
	},
	.quit_combo = GP_BIT(GP_F6),
};
