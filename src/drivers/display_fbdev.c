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
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "blit.h"
#include "console.h"
#include "core/log.h"

#define TAG "fbdev"

struct fbdev {
	struct hal_display base;
	struct hal_layer layer;
	int fd;
	uint8_t *map;
	size_t map_len;
	struct blit_dst front;	/* visible part of the mapping */
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int rotation;
	struct hal_fb shadow;
	struct damage damage;
};

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
	fb->front.pixels = fb->map + (size_t)fb->var.yoffset * fb->fix.line_length +
			   (size_t)fb->var.xoffset * (fb->var.bits_per_pixel / 8);
	fb->front.width = fb->var.xres;
	fb->front.height = fb->var.yres;
	fb->front.stride = fb->fix.line_length;

	fb->rotation = p->rotation == HAL_ROT_AUTO ? console_rotation() : (p->rotation & 3);
	fb->shadow.width = fb->rotation & 1 ? (int)fb->var.yres : (int)fb->var.xres;
	fb->shadow.height = fb->rotation & 1 ? (int)fb->var.xres : (int)fb->var.yres;
	fb->shadow.stride = fb->shadow.width * 4;
	fb->shadow.format = HAL_FMT_XRGB8888;
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

	damage_add(&fb->damage, r, fb->shadow.width, fb->shadow.height);
}

static void fbdev_submit(struct hal_layer *l)
{
	struct fbdev *fb = (struct fbdev *)l->display;
	enum blit_mode mode = fb->var.bits_per_pixel == 16 ? BLIT_RGB565 : BLIT_COPY;
	int i;

	for (i = 0; i < fb->damage.n; i++)
		blit_rotated(&fb->front, &fb->shadow, &fb->damage.r[i], fb->rotation, mode);
	fb->damage.n = 0;
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
