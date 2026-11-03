#include "display_drm.h"

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "blit.h"
#include "console.h"
#include "shot.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "core/util.h"

#define TAG "drm"
#define MAX_PLANES 8
#define MAX_COMMIT_PROPS 64

enum {
	PROP_FB_ID, PROP_CRTC_ID,
	PROP_SRC_X, PROP_SRC_Y, PROP_SRC_W, PROP_SRC_H,
	PROP_CRTC_X, PROP_CRTC_Y, PROP_CRTC_W, PROP_CRTC_H,
	PROP_TYPE,
	PROP_COUNT
};

static const char *const prop_names[PROP_COUNT] = {
	"FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
	"CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H", "type",
};

/* values of the "type" plane property */
enum { PLANE_OVERLAY, PLANE_PRIMARY, PLANE_CURSOR };

struct plane {
	uint32_t id;
	int type;
	bool xrgb, argb;
	bool used;
	uint32_t prop[PROP_COUNT];
};

struct dumb {
	uint32_t handle, fb_id, pitch;
	uint64_t size;
	uint8_t *map;
	struct damage damage;	/* areas this buffer still lacks */
};

struct drm_layer {
	struct hal_layer base;
	struct plane *plane;	/* NULL: no plane of its own */
	/*
	 * The video layer without a plane (the overlays take no RGB, e.g. the
	 * Amlogic video plane): only the shadow, put under the UI in the UI
	 * plane's buffers
	 */
	bool composed;
	struct hal_fb shadow;
	struct dumb buf[2];
	int front;		/* buffer on screen */
	bool dirty;		/* submitted, waiting for a commit */
	enum blit_mode mode;
};

struct drmdisp {
	struct hal_display base;
	struct mainloop *ml;
	int fd;
	uint32_t crtc_id;
	int crtc_index;
	int w, h;		/* mode size, native orientation */
	int rotation;
	struct plane planes[MAX_PLANES];
	int nplanes;
	struct drm_layer layers[LAYER_COUNT];
	struct hal_fb compose;	/* UI over the composed video, before turning */
	bool flip_pending;
	bool commit_wanted;
	bool commit_failed;	/* log the first failure only */
	unsigned commits, flips;
};

struct commit {
	uint32_t objs[MAX_PLANES], counts[MAX_PLANES];
	uint32_t props[MAX_COMMIT_PROPS];
	uint64_t values[MAX_COMMIT_PROPS];
	int nobjs, nprops;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do
		ret = ioctl(fd, req, arg);
	while (ret < 0 && (errno == EINTR || errno == EAGAIN));
	return ret;
}

static void *array(size_t n, size_t size)
{
	return calloc(n ? n : 1, size);
}

#define PTR(p) ((uint64_t)(uintptr_t)(p))

/* ---- probing ---------------------------------------------------------- */

static int find_crtc(struct drmdisp *d)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t *crtcs = NULL, *conns = NULL, *encs = NULL;
	int ret = -1;
	uint32_t i, j;

	if (xioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return -1;
	crtcs = array(res.count_crtcs, sizeof(*crtcs));
	conns = array(res.count_connectors, sizeof(*conns));
	encs = array(res.count_encoders, sizeof(*encs));
	res.crtc_id_ptr = PTR(crtcs);
	res.connector_id_ptr = PTR(conns);
	res.encoder_id_ptr = PTR(encs);
	res.count_fbs = 0;
	if (xioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		goto out;

	for (i = 0; i < res.count_connectors && ret < 0; i++) {
		struct drm_mode_get_connector conn = { .connector_id = conns[i] };
		struct drm_mode_get_encoder enc = { 0 };
		struct drm_mode_crtc crtc = { 0 };

		if (xioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 ||
		    conn.connection != 1 || !conn.encoder_id)
			continue;	/* not connected or not driven */
		enc.encoder_id = conn.encoder_id;
		if (xioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0 || !enc.crtc_id)
			continue;
		crtc.crtc_id = enc.crtc_id;
		if (xioctl(d->fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0 || !crtc.mode_valid)
			continue;	/* we only change planes, never the mode */
		for (j = 0; j < res.count_crtcs; j++)
			if (crtcs[j] == crtc.crtc_id)
				d->crtc_index = j;
		d->crtc_id = crtc.crtc_id;
		d->w = crtc.mode.hdisplay;
		d->h = crtc.mode.vdisplay;
		LOGI(TAG, "connector %u -> crtc %u, mode %s %ux%u@%u", conn.connector_id,
		     d->crtc_id, crtc.mode.name, d->w, d->h, crtc.mode.vrefresh);
		ret = 0;
	}
	if (ret < 0)
		LOGE(TAG, "no active output (the text console normally sets one up)");
out:
	free(crtcs);
	free(conns);
	free(encs);
	return ret;
}

static void plane_props(struct drmdisp *d, struct plane *p)
{
	struct drm_mode_obj_get_properties op = { .obj_id = p->id, .obj_type = DRM_MODE_OBJECT_PLANE };
	uint32_t *ids;
	uint64_t *vals;
	uint32_t i;
	int k;

	if (xioctl(d->fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) < 0)
		return;
	ids = array(op.count_props, sizeof(*ids));
	vals = array(op.count_props, sizeof(*vals));
	op.props_ptr = PTR(ids);
	op.prop_values_ptr = PTR(vals);
	if (xioctl(d->fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) == 0) {
		for (i = 0; i < op.count_props; i++) {
			struct drm_mode_get_property gp = { .prop_id = ids[i] };

			if (xioctl(d->fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0)
				continue;
			for (k = 0; k < PROP_COUNT; k++)
				if (!strcmp(gp.name, prop_names[k])) {
					p->prop[k] = ids[i];
					if (k == PROP_TYPE)
						p->type = (int)vals[i];
				}
		}
	}
	free(ids);
	free(vals);
}

static int find_planes(struct drmdisp *d)
{
	struct drm_mode_get_plane_res pr = { 0 };
	uint32_t *ids = NULL, i, j;
	int k;

	if (xioctl(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0)
		return -1;
	ids = array(pr.count_planes, sizeof(*ids));
	pr.plane_id_ptr = PTR(ids);
	if (xioctl(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) {
		free(ids);
		return -1;
	}
	for (i = 0; i < pr.count_planes && d->nplanes < MAX_PLANES; i++) {
		struct drm_mode_get_plane gp = { .plane_id = ids[i] };
		struct plane *p = &d->planes[d->nplanes];
		uint32_t *fmts;
		bool ok = true;

		if (xioctl(d->fd, DRM_IOCTL_MODE_GETPLANE, &gp) < 0 ||
		    !(gp.possible_crtcs & (1u << d->crtc_index)))
			continue;
		fmts = array(gp.count_format_types, sizeof(*fmts));
		gp.format_type_ptr = PTR(fmts);
		if (xioctl(d->fd, DRM_IOCTL_MODE_GETPLANE, &gp) == 0)
			for (j = 0; j < gp.count_format_types; j++) {
				p->xrgb |= fmts[j] == DRM_FORMAT_XRGB8888;
				p->argb |= fmts[j] == DRM_FORMAT_ARGB8888;
			}
		free(fmts);
		memset(p->prop, 0, sizeof(p->prop));
		p->id = ids[i];
		p->type = -1;
		plane_props(d, p);
		for (k = 0; k < PROP_TYPE; k++)
			ok &= p->prop[k] != 0;
		if (!ok || p->type < 0) {
			LOGW(TAG, "plane %u lacks atomic properties, skipped", p->id);
			continue;
		}
		LOGI(TAG, "plane %u: %s%s%s", p->id,
		     p->type == PLANE_PRIMARY ? "primary" : p->type == PLANE_CURSOR ? "cursor" : "overlay",
		     p->xrgb ? " XRGB8888" : "", p->argb ? " ARGB8888" : "");
		d->nplanes++;
	}
	free(ids);
	return d->nplanes ? 0 : -1;
}

static struct plane *take_plane(struct drmdisp *d, int type, bool need_argb)
{
	int i;

	for (i = 0; i < d->nplanes; i++) {
		struct plane *p = &d->planes[i];

		if (!p->used && p->type == type && (need_argb ? p->argb : p->xrgb)) {
			p->used = true;
			return p;
		}
	}
	return NULL;
}

/* ---- buffers ---------------------------------------------------------- */

static int dumb_create(struct drmdisp *d, struct dumb *b, uint32_t format)
{
	struct drm_mode_create_dumb cd = { .width = d->w, .height = d->h, .bpp = 32 };
	struct drm_mode_map_dumb md = { 0 };
	struct drm_mode_fb_cmd2 fb = { 0 };

	if (xioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
		return -1;
	b->handle = cd.handle;
	b->pitch = cd.pitch;
	b->size = cd.size;
	fb.width = d->w;
	fb.height = d->h;
	fb.pixel_format = format;
	fb.handles[0] = b->handle;
	fb.pitches[0] = b->pitch;
	if (xioctl(d->fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0)
		return -1;
	b->fb_id = fb.fb_id;
	md.handle = b->handle;
	if (xioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
		return -1;
	b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, md.offset);
	if (b->map == MAP_FAILED) {
		b->map = NULL;
		return -1;
	}
	memset(b->map, 0, b->size);	/* black, or fully transparent with alpha */
	return 0;
}

static void dumb_destroy(struct drmdisp *d, struct dumb *b)
{
	struct drm_mode_destroy_dumb dd = { .handle = b->handle };

	if (b->map)
		munmap(b->map, b->size);
	if (b->fb_id)
		xioctl(d->fd, DRM_IOCTL_MODE_RMFB, &b->fb_id);
	if (b->handle)
		xioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
	memset(b, 0, sizeof(*b));
}

/*
 * alpha: a shadow with alpha, premultiplied into the plane (over another
 * plane) or, without a plane under it, put over the composed video.
 * p NULL: the composed video layer, a shadow only.
 */
static int layer_setup(struct drmdisp *d, enum layer_role role, struct plane *p, bool alpha)
{
	struct drm_layer *l = &d->layers[role];
	struct hal_rect all;
	int i;

	l->base.display = &d->base;
	l->base.role = role;
	l->plane = p;
	l->composed = !p;
	l->shadow.width = d->rotation & 1 ? d->h : d->w;
	l->shadow.height = d->rotation & 1 ? d->w : d->h;
	l->shadow.stride = l->shadow.width * 4;
	l->shadow.format = alpha ? HAL_FMT_ARGB8888 : HAL_FMT_XRGB8888;
	l->shadow.pixels = calloc((size_t)l->shadow.width * l->shadow.height, 4);
	/* over a plane: premultiplied; over the composed video: blit_compose() */
	l->mode = alpha && d->layers[LAYER_VIDEO].plane ? BLIT_PREMULTIPLY : BLIT_COPY;
	if (!l->shadow.pixels)
		return -1;
	if (!p)
		return 0;
	all = (struct hal_rect){ 0, 0, l->shadow.width - 1, l->shadow.height - 1 };
	for (i = 0; i < 2; i++) {
		if (dumb_create(d, &l->buf[i], l->mode == BLIT_PREMULTIPLY ?
				DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888) < 0) {
			LOGE(TAG, "buffer for plane %u: %s", p->id, strerror(errno));
			return -1;
		}
		damage_add(&l->buf[i].damage, &all, l->shadow.width, l->shadow.height);
	}
	l->front = 1;		/* the first commit shows buffer 0 */
	l->dirty = true;
	return 0;
}

/* ---- commits ---------------------------------------------------------- */

static void add_prop(struct commit *c, uint32_t obj, uint32_t prop, uint64_t value)
{
	if (!c->nobjs || c->objs[c->nobjs - 1] != obj) {
		c->objs[c->nobjs] = obj;
		c->counts[c->nobjs++] = 0;
	}
	c->counts[c->nobjs - 1]++;
	c->props[c->nprops] = prop;
	c->values[c->nprops++] = value;
}

static void add_plane(struct drmdisp *d, struct commit *c, const struct plane *p, uint32_t fb_id)
{
	add_prop(c, p->id, p->prop[PROP_FB_ID], fb_id);
	add_prop(c, p->id, p->prop[PROP_CRTC_ID], fb_id ? d->crtc_id : 0);
	if (!fb_id)
		return;
	add_prop(c, p->id, p->prop[PROP_SRC_X], 0);
	add_prop(c, p->id, p->prop[PROP_SRC_Y], 0);
	add_prop(c, p->id, p->prop[PROP_SRC_W], (uint64_t)d->w << 16);
	add_prop(c, p->id, p->prop[PROP_SRC_H], (uint64_t)d->h << 16);
	add_prop(c, p->id, p->prop[PROP_CRTC_X], 0);
	add_prop(c, p->id, p->prop[PROP_CRTC_Y], 0);
	add_prop(c, p->id, p->prop[PROP_CRTC_W], d->w);
	add_prop(c, p->id, p->prop[PROP_CRTC_H], d->h);
}

static int run_commit(struct drmdisp *d, struct commit *c, uint32_t flags)
{
	struct drm_mode_atomic a = {
		.flags = flags,
		.count_objs = c->nobjs,
		.objs_ptr = PTR(c->objs),
		.count_props_ptr = PTR(c->counts),
		.props_ptr = PTR(c->props),
		.prop_values_ptr = PTR(c->values),
	};

	return ioctl(d->fd, DRM_IOCTL_MODE_ATOMIC, &a);
}

/* Copy what the back buffers lack and flip every dirty layer at once */
static void flush(struct drmdisp *d)
{
	struct commit c = { 0 };
	int back[LAYER_COUNT] = { 0 }, i, r;

	if (d->flip_pending) {
		d->commit_wanted = true;
		return;
	}
	for (i = 0; i < LAYER_COUNT; i++) {
		struct drm_layer *l = &d->layers[i];
		struct dumb *b;
		struct blit_dst dst;

		if (!l->plane || !l->dirty)
			continue;
		back[i] = !l->front;
		b = &l->buf[back[i]];
		dst = (struct blit_dst){ b->map, d->w, d->h, (int)b->pitch };
		for (r = 0; r < b->damage.n; r++) {
			const struct hal_fb *src = &l->shadow;

			if (i == LAYER_UI && d->layers[LAYER_VIDEO].composed) {
				blit_compose(&d->compose, &l->shadow, &d->layers[LAYER_VIDEO].shadow,
					     &b->damage.r[r]);
				src = &d->compose;
			}
			blit_rotated(&dst, src, &b->damage.r[r], d->rotation, l->mode);
		}
		b->damage.n = 0;
		add_plane(d, &c, l->plane, b->fb_id);
	}
	if (!c.nobjs)
		return;
	if (run_commit(d, &c, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT) < 0) {
		if (!d->commit_failed)
			LOGE(TAG, "atomic commit: %s", strerror(errno));
		d->commit_failed = true;
		return;		/* layers stay dirty, retried on the next submit */
	}
	d->flip_pending = true;
	d->commits++;
	for (i = 0; i < LAYER_COUNT; i++) {
		struct drm_layer *l = &d->layers[i];

		if (l->plane && l->dirty) {
			l->front = back[i];
			l->dirty = false;
		}
	}
}

static void on_drm_event(int fd, short revents, void *ud)
{
	struct drmdisp *d = ud;
	char buf[1024];
	ssize_t n, off;

	(void)revents;
	n = read(fd, buf, sizeof(buf));
	for (off = 0; n > 0 && off + (ssize_t)sizeof(struct drm_event) <= n;) {
		struct drm_event *e = (struct drm_event *)(buf + off);

		if (e->type == DRM_EVENT_FLIP_COMPLETE) {
			d->flip_pending = false;
			d->flips++;
		}
		off += e->length;
	}
	if (!d->flip_pending && d->commit_wanted) {
		d->commit_wanted = false;
		flush(d);
	}
}

/* ---- hal_display_ops -------------------------------------------------- */

static void drm_free(struct drmdisp *d)
{
	int i, j;

	for (i = 0; i < LAYER_COUNT; i++) {
		for (j = 0; j < 2; j++)
			dumb_destroy(d, &d->layers[i].buf[j]);
		free(d->layers[i].shadow.pixels);
	}
	free(d->compose.pixels);
	if (d->fd >= 0)
		close(d->fd);
	free(d);
}

static struct hal_display *drm_open(const void *params, struct mainloop *ml)
{
	const struct drm_params *p = params;
	struct drm_set_client_cap cap;
	struct plane *video = NULL, *ui;
	struct drmdisp *d;
	struct commit c = { 0 };
	int i;

	d = calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->base.ops = &display_drm;
	d->ml = ml;
	d->fd = open(p->device, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (d->fd < 0) {
		LOGW(TAG, "%s: %s", p->device, strerror(errno));
		goto fail;
	}
	cap = (struct drm_set_client_cap){ DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
	xioctl(d->fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
	cap = (struct drm_set_client_cap){ DRM_CLIENT_CAP_ATOMIC, 1 };
	if (xioctl(d->fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
		LOGW(TAG, "%s: no atomic modesetting", p->device);
		goto fail;
	}
	if (ioctl(d->fd, DRM_IOCTL_SET_MASTER, 0) < 0 && errno != EINVAL)
		LOGW(TAG, "SET_MASTER: %s (another DRM master running?)", strerror(errno));
	if (find_crtc(d) < 0 || find_planes(d) < 0)
		goto fail;
	d->rotation = p->rotation == HAL_ROT_AUTO ? console_rotation() : (p->rotation & 3);

	/*
	 * UI on an overlay with alpha over the primary (video); else UI on the
	 * primary, the video put under it by the CPU
	 */
	ui = take_plane(d, PLANE_OVERLAY, true);
	if (ui) {
		video = take_plane(d, PLANE_PRIMARY, false);
	} else {
		ui = take_plane(d, PLANE_PRIMARY, false);
		if (!ui) {
			LOGE(TAG, "no usable plane");
			goto fail;
		}
	}
	if (layer_setup(d, LAYER_VIDEO, video, false) < 0 ||
	    layer_setup(d, LAYER_UI, ui, true) < 0)
		goto fail;
	if (!video) {
		d->compose = d->layers[LAYER_UI].shadow;
		d->compose.format = HAL_FMT_XRGB8888;
		d->compose.pixels = calloc((size_t)d->compose.width * d->compose.height, 4);
		if (!d->compose.pixels)
			goto fail;
	}

	/* First commit, blocking: our buffers on, all other planes off */
	for (i = 0; i < d->nplanes; i++)
		if (!d->planes[i].used)
			add_plane(d, &c, &d->planes[i], 0);
	for (i = 0; i < LAYER_COUNT; i++)
		if (d->layers[i].plane) {
			add_plane(d, &c, d->layers[i].plane, d->layers[i].buf[0].fb_id);
			d->layers[i].front = 0;
			d->layers[i].buf[0].damage.n = 0;	/* zeroed = what the shadow holds */
			d->layers[i].dirty = false;
		}
	if (run_commit(d, &c, 0) < 0) {
		LOGE(TAG, "first commit: %s", strerror(errno));
		goto fail;
	}
	mainloop_add_fd(ml, d->fd, POLLIN, on_drm_event, d);
	LOGI(TAG, "logical %dx%d, rotation %d; UI on plane %u (alpha), video %s",
	     d->layers[LAYER_UI].shadow.width, d->layers[LAYER_UI].shadow.height, d->rotation, ui->id,
	     video ? "on the primary plane" : "composed under the UI");
	console_graphics(p->tty);
	return &d->base;
fail:
	drm_free(d);
	return NULL;
}

static void drm_close(struct hal_display *disp)
{
	struct drmdisp *d = (struct drmdisp *)disp;

	LOGI(TAG, "%u commits, %u flips", d->commits, d->flips);
	mainloop_remove_fd(d->ml, d->fd);
	/* closing the last fd lets the kernel's fbdev client restore the console */
	drm_free(d);
	console_restore();
}

#ifdef CONFIG_SNAPSHOT
static int drm_snapshot(struct hal_display *disp, const char *path)
{
	struct drmdisp *d = (struct drmdisp *)disp;

	return shot_write_ppm(path, &d->layers[LAYER_UI].shadow, &d->layers[LAYER_VIDEO].shadow);
}
#endif

static void drm_info(struct hal_display *disp, struct hal_display_info *info)
{
	struct drmdisp *d = (struct drmdisp *)disp;

	info->width = d->layers[LAYER_UI].shadow.width;
	info->height = d->layers[LAYER_UI].shadow.height;
	info->rotation = d->rotation;
}

static struct hal_layer *drm_layer(struct hal_display *disp, enum layer_role role)
{
	struct drmdisp *d = (struct drmdisp *)disp;

	return d->layers[role].plane || d->layers[role].composed ? &d->layers[role].base : NULL;
}

static struct hal_fb *drm_acquire(struct hal_layer *l)
{
	return &((struct drm_layer *)l)->shadow;
}

/* The composed video layer lands in the UI plane's buffers */
static struct drm_layer *target(struct hal_layer *hl)
{
	struct drm_layer *l = (struct drm_layer *)hl;

	return l->composed ? &((struct drmdisp *)hl->display)->layers[LAYER_UI] : l;
}

static void drm_damage(struct hal_layer *hl, const struct hal_rect *r)
{
	struct drm_layer *l = target(hl);

	damage_add(&l->buf[0].damage, r, l->shadow.width, l->shadow.height);
	damage_add(&l->buf[1].damage, r, l->shadow.width, l->shadow.height);
}

static void drm_submit(struct hal_layer *hl)
{
	target(hl)->dirty = true;
	flush((struct drmdisp *)hl->display);
}

const struct hal_display_ops display_drm = {
	.name = "drm",
	.open = drm_open,
	.close = drm_close,
	.info = drm_info,
	.layer = drm_layer,
	.layer_acquire = drm_acquire,
	.layer_damage = drm_damage,
	.layer_submit = drm_submit,
#ifdef CONFIG_SNAPSHOT
	.snapshot = drm_snapshot,
#endif
};
