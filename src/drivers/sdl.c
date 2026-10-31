#include "sdl.h"

#include <SDL2/SDL.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "core/input.h"
#include "core/log.h"
#include "core/mainloop.h"

#define TAG "sdl"
#define PUMP_MS 10

struct sdl_display {
	struct hal_display base;
	struct hal_layer layer;
	struct hal_fb fb;
	SDL_Window *win;
	SDL_Renderer *ren;
	SDL_Texture *tex;
	bool dirty;
};

static struct hal_display *sdl_open(const void *params, struct mainloop *ml)
{
	const struct sdl_params *p = params;
	struct sdl_display *s;
	int zoom = p->zoom > 0 ? p->zoom : 1;

	(void)ml;
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		LOGE(TAG, "SDL_Init: %s", SDL_GetError());
		return NULL;
	}
	s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->base.ops = &display_sdl;
	s->layer.display = &s->base;
	s->layer.role = LAYER_UI;
	s->fb.width = p->width;
	s->fb.height = p->height;
	s->fb.stride = p->width * 4;
	s->fb.pixels = calloc((size_t)p->width * p->height, 4);
	s->win = SDL_CreateWindow("bare_app", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
				  p->width * zoom, p->height * zoom, 0);
	s->ren = s->win ? SDL_CreateRenderer(s->win, -1, 0) : NULL;
	s->tex = s->ren ? SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_ARGB8888,
					    SDL_TEXTUREACCESS_STREAMING, p->width, p->height) : NULL;
	if (!s->fb.pixels || !s->tex) {
		LOGE(TAG, "window: %s", SDL_GetError());
		free(s->fb.pixels);
		free(s);
		SDL_Quit();
		return NULL;
	}
	SDL_RenderSetLogicalSize(s->ren, p->width, p->height);
	LOGI(TAG, "window %dx%d, zoom %d", p->width, p->height, zoom);
	return &s->base;
}

static void sdl_close(struct hal_display *d)
{
	struct sdl_display *s = (struct sdl_display *)d;

	SDL_DestroyTexture(s->tex);
	SDL_DestroyRenderer(s->ren);
	SDL_DestroyWindow(s->win);
	SDL_Quit();
	free(s->fb.pixels);
	free(s);
}

static void sdl_info(struct hal_display *d, struct hal_display_info *info)
{
	struct sdl_display *s = (struct sdl_display *)d;

	info->width = s->fb.width;
	info->height = s->fb.height;
	info->rotation = HAL_ROT_0;
}

static struct hal_layer *sdl_layer(struct hal_display *d, enum layer_role role)
{
	return role == LAYER_UI ? &((struct sdl_display *)d)->layer : NULL;
}

static struct hal_fb *sdl_acquire(struct hal_layer *l)
{
	return &((struct sdl_display *)l->display)->fb;
}

static void sdl_damage(struct hal_layer *l, const struct hal_rect *r)
{
	(void)r;
	((struct sdl_display *)l->display)->dirty = true;
}

static void sdl_submit(struct hal_layer *l)
{
	struct sdl_display *s = (struct sdl_display *)l->display;

	if (!s->dirty)
		return;
	s->dirty = false;
	SDL_UpdateTexture(s->tex, NULL, s->fb.pixels, s->fb.stride);
	SDL_RenderClear(s->ren);
	SDL_RenderCopy(s->ren, s->tex, NULL, NULL);
	SDL_RenderPresent(s->ren);
}

const struct hal_display_ops display_sdl = {
	.name = "sdl",
	.open = sdl_open,
	.close = sdl_close,
	.info = sdl_info,
	.layer = sdl_layer,
	.layer_acquire = sdl_acquire,
	.layer_damage = sdl_damage,
	.layer_submit = sdl_submit,
};

/* ---- keyboard --------------------------------------------------------- */

struct sdl_input {
	struct mainloop *ml;
	struct ml_timer *pump;
};

static enum gp_button map_key(SDL_Keycode k)
{
	switch (k) {
	case SDLK_UP:        return GP_UP;
	case SDLK_DOWN:      return GP_DOWN;
	case SDLK_LEFT:      return GP_LEFT;
	case SDLK_RIGHT:     return GP_RIGHT;
	case SDLK_x:         return GP_A;
	case SDLK_z:
	case SDLK_ESCAPE:    return GP_B;
	case SDLK_s:         return GP_X;
	case SDLK_a:         return GP_Y;
	case SDLK_q:         return GP_L1;
	case SDLK_w:         return GP_R1;
	case SDLK_RETURN:    return GP_START;
	case SDLK_BACKSPACE: return GP_SELECT;
	case SDLK_1: case SDLK_2: case SDLK_3:
	case SDLK_4: case SDLK_5: case SDLK_6:
		return GP_F1 + (k - SDLK_1);
	default:             return GP_NONE;
	}
}

static void pump(void *ud)
{
	SDL_Event e;

	(void)ud;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) {
			kill(getpid(), SIGTERM);	/* handled by the control thread */
		} else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
			enum gp_button b = map_key(e.key.keysym.sym);

			if (b != GP_NONE)
				input_report_button(b, e.type == SDL_KEYDOWN);
		}
	}
}

static void *sdl_input_open(const void *params, struct mainloop *ml)
{
	struct sdl_input *in;

	(void)params;
	if (!SDL_WasInit(SDL_INIT_VIDEO))
		return NULL;	/* SDL keyboard needs the SDL window */
	in = calloc(1, sizeof(*in));
	if (!in)
		return NULL;
	in->ml = ml;
	in->pump = mainloop_add_timer(ml, 0, PUMP_MS, pump, in);
	input_declare((GP_BIT(GP_COUNT) - 1) & ~(GP_BIT(GP_L2) | GP_BIT(GP_R2) | GP_BIT(GP_MENU) |
					    GP_BIT(GP_VOL_UP) | GP_BIT(GP_VOL_DOWN)), 0);
	return in;
}

static void sdl_input_close(void *h)
{
	struct sdl_input *in = h;

	mainloop_timer_cancel(in->ml, in->pump);
	free(in);
}

const struct hal_input_ops input_sdl = {
	.name = "sdl",
	.open = sdl_input_open,
	.close = sdl_input_close,
};
