/*
 * Linux framebuffer display: one layer (LAYER_UI).
 *
 * The toolkit draws into a shadow buffer in logical orientation; submit
 * copies the damaged rectangles to /dev/fb0, turned the same way fbcon
 * turns the text console. The panel of the OGA is mounted rotated, the
 * framebuffer keeps the native portrait orientation.
 */
#include "display_fbdev.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"

#define TAG "fbdev"
#define MAX_DAMAGE 16

struct fbdev {
	struct hal_display base;
	struct hal_layer layer;
	int fd;
	uint8_t *map;
	size_t map_len;
	uint8_t *front;		/* visible part of the mapping */
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int rotation;
	struct hal_fb shadow;
	struct hal_rect damage[MAX_DAMAGE];
	int ndamage;
};

/* Restoring the text console must survive a crash, so it is global */
static volatile int console_fd = -1;

static void console_restore(void)
{
	if (console_fd >= 0)
		ioctl(console_fd, KDSETMODE, KD_TEXT);
}

static void on_fatal(int sig)
{
	console_restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

static void console_graphics(const char *tty)
{
	static const int fatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
	size_t i;
	int fd;

	if (!tty)
		return;
	fd = open(tty, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		LOGW(TAG, "%s: %s, the console may draw over the screen", tty, strerror(errno));
		return;
	}
	if (ioctl(fd, KDSETMODE, KD_GRAPHICS) < 0) {
		LOGW(TAG, "KD_GRAPHICS on %s: %s", tty, strerror(errno));
		close(fd);
		return;
	}
	console_fd = fd;
	atexit(console_restore);
	for (i = 0; i < ARRAY_SIZE(fatal); i++)
		signal(fatal[i], on_fatal);
}

static int fbcon_rotation(void)
{
	int r = 0;

	if (read_int_file("/sys/class/graphics/fbcon/rotate", &r) < 0)
		r = 0;
	return r & 3;
}

static struct hal_display *fbdev_open(const void *params, struct mainloop *ml)
{
	const struct fbdev_params *p = params;
	struct fbdev *fb;

	(void)ml;
	fb = calloc(1, sizeof(*fb));
	if (!fb)
		return NULL;
	fb->base.ops = &display_fbdev;
	fb->layer.display = &fb->base;
	fb->layer.role = LAYER_UI;
	fb->fd = open(p->device, O_RDWR | O_CLOEXEC);
	if (fb->fd < 0 || ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0 ||
	    ioctl(fb->fd, FBIOGET_FSCREENINFO, &fb->fix) < 0) {
		LOGE(TAG, "%s: %s", p->device, strerror(errno));
		goto fail;
	}
	if (fb->var.bits_per_pixel != 32 && fb->var.bits_per_pixel != 16) {
		LOGE(TAG, "%u bpp is not supported", fb->var.bits_per_pixel);
		goto fail;
	}
	fb->map_len = fb->fix.smem_len;
	fb->map = mmap(NULL, fb->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if (fb->map == MAP_FAILED) {
		LOGE(TAG, "mmap: %s", strerror(errno));
		fb->map = NULL;
		goto fail;
	}
	fb->front = fb->map + (size_t)fb->var.yoffset * fb->fix.line_length +
		    (size_t)fb->var.xoffset * (fb->var.bits_per_pixel / 8);

	fb->rotation = p->rotation == HAL_ROT_AUTO ? fbcon_rotation() : (p->rotation & 3);
	fb->shadow.width = fb->rotation & 1 ? (int)fb->var.yres : (int)fb->var.xres;
	fb->shadow.height = fb->rotation & 1 ? (int)fb->var.xres : (int)fb->var.yres;
	fb->shadow.stride = fb->shadow.width * 4;
	fb->shadow.pixels = calloc((size_t)fb->shadow.width * fb->shadow.height, 4);
	if (!fb->shadow.pixels)
		goto fail;

	LOGI(TAG, "%s: %ux%u %u bpp, line %u; logical %dx%d, rotation %d", p->device,
	     fb->var.xres, fb->var.yres, fb->var.bits_per_pixel, fb->fix.line_length,
	     fb->shadow.width, fb->shadow.height, fb->rotation);
	console_graphics(p->tty);
	return &fb->base;
fail:
	if (fb->map)
		munmap(fb->map, fb->map_len);
	if (fb->fd >= 0)
		close(fb->fd);
	free(fb);
	return NULL;
}

static void fbdev_close(struct hal_display *d)
{
	struct fbdev *fb = (struct fbdev *)d;

	console_restore();
	munmap(fb->map, fb->map_len);
	close(fb->fd);
	free(fb->shadow.pixels);
	free(fb);
}

static void fbdev_info(struct hal_display *d, struct hal_display_info *info)
{
	struct fbdev *fb = (struct fbdev *)d;

	info->width = fb->shadow.width;
	info->height = fb->shadow.height;
	info->rotation = fb->rotation;
}

static struct hal_layer *fbdev_layer(struct hal_display *d, enum layer_role role)
{
	struct fbdev *fb = (struct fbdev *)d;

	return role == LAYER_UI ? &fb->layer : NULL;
}

static struct hal_fb *fbdev_acquire(struct hal_layer *l)
{
	return &((struct fbdev *)l->display)->shadow;
}

static void fbdev_damage(struct hal_layer *l, const struct hal_rect *r)
{
	struct fbdev *fb = (struct fbdev *)l->display;
	struct hal_rect c = {
		MAX(r->x1, 0), MAX(r->y1, 0),
		MIN(r->x2, fb->shadow.width - 1), MIN(r->y2, fb->shadow.height - 1),
	};

	if (c.x1 > c.x2 || c.y1 > c.y2)
		return;
	if (fb->ndamage == MAX_DAMAGE) {
		/* too many pieces: merge everything into one box */
		struct hal_rect *b = &fb->damage[0];
		int i;

		for (i = 1; i < fb->ndamage; i++) {
			b->x1 = MIN(b->x1, fb->damage[i].x1);
			b->y1 = MIN(b->y1, fb->damage[i].y1);
			b->x2 = MAX(b->x2, fb->damage[i].x2);
			b->y2 = MAX(b->y2, fb->damage[i].y2);
		}
		fb->ndamage = 1;
	}
	fb->damage[fb->ndamage++] = c;
}

static inline uint16_t to_rgb565(uint32_t c)
{
	return ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f);
}

/* Logical (sx, sy) -> framebuffer (x, y), as fbcon=rotate:N does */
static inline void map_xy(const struct fbdev *fb, int sx, int sy, int *x, int *y)
{
	switch (fb->rotation) {
	case 1:  *x = fb->var.xres - 1 - sy; *y = sx; break;
	case 2:  *x = fb->var.xres - 1 - sx; *y = fb->var.yres - 1 - sy; break;
	case 3:  *x = sy; *y = fb->var.yres - 1 - sx; break;
	default: *x = sx; *y = sy; break;
	}
}

static void blit(struct fbdev *fb, const struct hal_rect *r)
{
	const uint32_t *src = fb->shadow.pixels;
	int w = fb->shadow.width, sx, sy, x, y;
	size_t line = fb->fix.line_length;

	if (fb->var.bits_per_pixel == 32) {
		for (sy = r->y1; sy <= r->y2; sy++) {
			const uint32_t *s = src + (size_t)sy * w;

			for (sx = r->x1; sx <= r->x2; sx++) {
				map_xy(fb, sx, sy, &x, &y);
				*(uint32_t *)(fb->front + y * line + (size_t)x * 4) = s[sx];
			}
		}
	} else {
		for (sy = r->y1; sy <= r->y2; sy++) {
			const uint32_t *s = src + (size_t)sy * w;

			for (sx = r->x1; sx <= r->x2; sx++) {
				map_xy(fb, sx, sy, &x, &y);
				*(uint16_t *)(fb->front + y * line + (size_t)x * 2) = to_rgb565(s[sx]);
			}
		}
	}
}

static void fbdev_submit(struct hal_layer *l)
{
	struct fbdev *fb = (struct fbdev *)l->display;
	int i;

	for (i = 0; i < fb->ndamage; i++)
		blit(fb, &fb->damage[i]);
	fb->ndamage = 0;
}

const struct hal_display_ops display_fbdev = {
	.name = "fbdev",
	.open = fbdev_open,
	.close = fbdev_close,
	.info = fbdev_info,
	.layer = fbdev_layer,
	.layer_acquire = fbdev_acquire,
	.layer_damage = fbdev_damage,
	.layer_submit = fbdev_submit,
};
