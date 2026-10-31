/*
 * Host PC.
 *
 *   pc      SDL window and keyboard when built with SDL2, otherwise the
 *           display in memory; input script from stdin in both cases
 *   pc-mem  always the display in memory, with a video layer under the UI
 *           like the OGA: tests and scripted screenshots without a window
 *           (BARE_APP_SHOT=file.ppm)
 */
#include <stddef.h>

#include "drivers/display_mem.h"
#include "drivers/input_script.h"
#include "hal/platform.h"

#ifdef BARE_APP_WITH_SDL
#include "drivers/sdl.h"
#endif

static const struct mem_display_params mem_params = { .width = 480, .height = 320 };
/* like the OGA with DRM planes: video under a UI with alpha */
static const struct mem_display_params mem_layers_params = {
	.width = 480, .height = 320, .video_layer = true,
};
static const struct script_params script_params = { .interval_ms = 150 };

const struct platform platform_pc = {
	.name = "pc",
	.compatible = NULL,
	.displays = {
#ifdef BARE_APP_WITH_SDL
		{ &display_sdl, &(const struct sdl_params){ .width = 480, .height = 320, .zoom = 2 } },
#endif
		{ &display_mem, &mem_params },
	},
	.inputs = {
#ifdef BARE_APP_WITH_SDL
		{ &input_sdl, NULL },
#endif
		{ &input_script, &script_params },
	},
	.quit_combo = GP_BIT(GP_F6),
	.config_path = "~/.config/bare_app.conf",
};

const struct platform platform_pc_mem = {
	.name = "pc-mem",
	.compatible = NULL,
	.displays = { { &display_mem, &mem_layers_params } },
	.inputs = {
		{ &input_script, &script_params },
	},
	.quit_combo = GP_BIT(GP_F6),
};
