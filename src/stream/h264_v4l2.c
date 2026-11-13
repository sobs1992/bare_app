#include "h264_v4l2.h"

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
#include <linux/videodev2.h>

#include "core/log.h"

#define TAG "h264"

#define NBUF 2
#define TIMEOUT_MS 1000

struct vbuf {
	void *p;
	size_t len;
};

struct h264_v4l2 {
	int fd;
	char name[80];
	int width, height, bpl;
	struct vbuf out[NBUF], cap[NBUF];
	int nout, ncap, next_out;
	uint8_t *au;			/* the last access unit */
	size_t au_size, au_cap;
	uint8_t params[256];		/* SPS and PPS of the last IDR */
	size_t params_size;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

static bool has_fmt(int fd, enum v4l2_buf_type type, uint32_t fourcc)
{
	struct v4l2_fmtdesc d = { .type = type };

	for (d.index = 0; !xioctl(fd, VIDIOC_ENUM_FMT, &d); d.index++)
		if (d.pixelformat == fourcc)
			return true;
	return false;
}

/* a single-plane mem2mem device turning XBGR32 into H.264 */
static int find_encoder(char *name, size_t size)
{
	struct v4l2_capability cap;
	char path[32];
	int i, fd;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!xioctl(fd, VIDIOC_QUERYCAP, &cap) &&
		    ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities) &
		    V4L2_CAP_VIDEO_M2M &&
		    has_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_H264) &&
		    has_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_PIX_FMT_XBGR32)) {
			snprintf(name, size, "%s (%s)", path, (const char *)cap.driver);
			return fd;
		}
		close(fd);
	}
	return -1;
}

static void set_ctrl(struct h264_v4l2 *e, uint32_t id, int val)
{
	struct v4l2_control c = { .id = id, .value = val };

	if (xioctl(e->fd, VIDIOC_S_CTRL, &c) < 0)
		LOGW(TAG, "%s: control %#x: %s", e->name, id, strerror(errno));
}

static int map_bufs(struct h264_v4l2 *e, enum v4l2_buf_type type, struct vbuf *b, int *n)
{
	struct v4l2_requestbuffers rb = { .count = NBUF, .type = type, .memory = V4L2_MEMORY_MMAP };
	unsigned int i;

	if (xioctl(e->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 1)
		return -1;
	*n = (int)(rb.count < NBUF ? rb.count : NBUF);
	for (i = 0; i < (unsigned int)*n; i++) {
		struct v4l2_buffer vb = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };

		if (xioctl(e->fd, VIDIOC_QUERYBUF, &vb) < 0)
			return -1;
		b[i].len = vb.length;
		b[i].p = mmap(NULL, vb.length, PROT_READ | PROT_WRITE, MAP_SHARED, e->fd, vb.m.offset);
		if (b[i].p == MAP_FAILED) {
			b[i].p = NULL;
			return -1;
		}
	}
	return 0;
}

static int qbuf(struct h264_v4l2 *e, enum v4l2_buf_type type, int index, size_t used)
{
	struct v4l2_buffer vb = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = (uint32_t)index,
				  .bytesused = (uint32_t)used };

	return xioctl(e->fd, VIDIOC_QBUF, &vb);
}

static int dqbuf(struct h264_v4l2 *e, enum v4l2_buf_type type, struct v4l2_buffer *vb)
{
	struct pollfd p = { .fd = e->fd, .events = type == V4L2_BUF_TYPE_VIDEO_CAPTURE ? POLLIN : POLLOUT };

	memset(vb, 0, sizeof(*vb));
	vb->type = type;
	vb->memory = V4L2_MEMORY_MMAP;
	for (;;) {
		if (!xioctl(e->fd, VIDIOC_DQBUF, vb))
			return 0;
		if (errno != EAGAIN)
			return -1;
		if (poll(&p, 1, TIMEOUT_MS) <= 0)
			return -1;
	}
}

/* what comes before the first slice of an IDR: its SPS and PPS */
static void keep_params(struct h264_v4l2 *e, const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i + 3 < n; i++) {
		if (p[i] || p[i + 1] || p[i + 2] != 1)
			continue;
		if ((p[i + 3] & 0x1f) == 5 || (p[i + 3] & 0x1f) == 1) {
			size_t end = (i && !p[i - 1]) ? i - 1 : i;

			if (end && end <= sizeof(e->params)) {
				memcpy(e->params, p, end);
				e->params_size = end;
			}
			return;
		}
	}
}

int h264_v4l2_encode(struct h264_v4l2 *e, const struct hal_fb *frame, const uint8_t **data,
		     size_t *size, int *idr)
{
	struct v4l2_buffer cb, ob;
	uint8_t *dst;
	int i = e->next_out, y;

	if (frame->width < e->width || frame->height < e->height)
		return -1;
	dst = e->out[i].p;
	for (y = 0; y < e->height; y++)
		memcpy(dst + (size_t)y * e->bpl,
		       (const uint8_t *)frame->pixels + (size_t)y * frame->stride, (size_t)e->width * 4);
	if (qbuf(e, V4L2_BUF_TYPE_VIDEO_OUTPUT, i, e->out[i].len) < 0) {
		LOGW(TAG, "%s: queue a frame: %s", e->name, strerror(errno));
		return -1;
	}
	e->next_out = (i + 1) % e->nout;
	if (dqbuf(e, V4L2_BUF_TYPE_VIDEO_CAPTURE, &cb) < 0) {
		LOGW(TAG, "%s: no picture: %s", e->name, strerror(errno));
		return -1;
	}
	if (cb.flags & V4L2_BUF_FLAG_ERROR || cb.index >= (uint32_t)e->ncap) {
		qbuf(e, V4L2_BUF_TYPE_VIDEO_CAPTURE, (int)cb.index, 0);
		dqbuf(e, V4L2_BUF_TYPE_VIDEO_OUTPUT, &ob);
		return -1;
	}
	if (cb.bytesused > e->au_cap) {
		uint8_t *p = realloc(e->au, cb.bytesused);

		if (!p)
			return -1;
		e->au = p;
		e->au_cap = cb.bytesused;
	}
	memcpy(e->au, e->cap[cb.index].p, cb.bytesused);
	e->au_size = cb.bytesused;
	qbuf(e, V4L2_BUF_TYPE_VIDEO_CAPTURE, (int)cb.index, 0);
	dqbuf(e, V4L2_BUF_TYPE_VIDEO_OUTPUT, &ob);

	*idr = !!(cb.flags & V4L2_BUF_FLAG_KEYFRAME);
	if (*idr)
		keep_params(e, e->au, e->au_size);
	*data = e->au;
	*size = e->au_size;
	return 0;
}

int h264_v4l2_params(struct h264_v4l2 *e, const uint8_t **data, size_t *size)
{
	if (!e->params_size)
		return -1;
	*data = e->params;
	*size = e->params_size;
	return 0;
}

void h264_v4l2_force_idr(struct h264_v4l2 *e)
{
	set_ctrl(e, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0);
}

void h264_v4l2_set_bitrate(struct h264_v4l2 *e, int kbps)
{
	set_ctrl(e, V4L2_CID_MPEG_VIDEO_BITRATE, kbps * 1000);
}

const char *h264_v4l2_name(const struct h264_v4l2 *e)
{
	return e->name;
}

struct h264_v4l2 *h264_v4l2_new(int width, int height, int fps, int kbps)
{
	struct h264_v4l2 *e = calloc(1, sizeof(*e));
	struct v4l2_format f;
	struct v4l2_streamparm parm;
	struct hal_fb black;
	const uint8_t *au;
	size_t n;
	int i, idr;
	enum v4l2_buf_type t;

	if (!e)
		return NULL;
	e->fd = find_encoder(e->name, sizeof(e->name));
	if (e->fd < 0) {
		free(e);
		return NULL;
	}

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	f.fmt.pix.width = (uint32_t)width;
	f.fmt.pix.height = (uint32_t)height;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_XBGR32;
	if (xioctl(e->fd, VIDIOC_S_FMT, &f) < 0 || f.fmt.pix.width != (uint32_t)width ||
	    f.fmt.pix.height != (uint32_t)height || f.fmt.pix.pixelformat != V4L2_PIX_FMT_XBGR32) {
		LOGW(TAG, "%s takes no %dx%d XRGB", e->name, width, height);
		goto fail;
	}
	e->width = width;
	e->height = height;
	e->bpl = (int)f.fmt.pix.bytesperline;

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	if (xioctl(e->fd, VIDIOC_S_FMT, &f) < 0)
		goto fail;

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	parm.parm.output.timeperframe.numerator = 1;
	parm.parm.output.timeperframe.denominator = (uint32_t)fps;
	xioctl(e->fd, VIDIOC_S_PARM, &parm);
	set_ctrl(e, V4L2_CID_MPEG_VIDEO_BITRATE, kbps * 1000);
	set_ctrl(e, V4L2_CID_MPEG_VIDEO_GOP_SIZE, fps * 2);	/* as the OpenH264 one */
	set_ctrl(e, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1);
	/*
	 * A still menu would take the QP down to the lowest, and the key frame
	 * after it (a game starting) would come out huge: a burst the Wi-Fi
	 * link stalls on. QP 20 looks the same on this screen.
	 */
	set_ctrl(e, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 20);

	if (map_bufs(e, V4L2_BUF_TYPE_VIDEO_OUTPUT, e->out, &e->nout) < 0 ||
	    map_bufs(e, V4L2_BUF_TYPE_VIDEO_CAPTURE, e->cap, &e->ncap) < 0) {
		LOGW(TAG, "%s: buffers: %s", e->name, strerror(errno));
		goto fail;
	}
	for (i = 0; i < e->ncap; i++)
		if (qbuf(e, V4L2_BUF_TYPE_VIDEO_CAPTURE, i, 0) < 0)
			goto fail;
	t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (xioctl(e->fd, VIDIOC_STREAMON, &t) < 0)
		goto fail;
	t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(e->fd, VIDIOC_STREAMON, &t) < 0)
		goto fail;

	/* SPS and PPS for the SDP before the first frame: one black picture */
	black.width = width;
	black.height = height;
	black.stride = width * 4;
	black.format = 0;
	black.pixels = calloc((size_t)width * height, 4);
	if (!black.pixels)
		goto fail;
	i = h264_v4l2_encode(e, &black, &au, &n, &idr);
	free(black.pixels);
	if (i < 0 || !e->params_size) {
		LOGW(TAG, "%s: no SPS and PPS", e->name);
		goto fail;
	}
	h264_v4l2_force_idr(e);
	LOGI(TAG, "%dx%d, %d fps, %d kbit/s in hardware: %s", width, height, fps, kbps, e->name);
	return e;
fail:
	h264_v4l2_free(e);
	return NULL;
}

void h264_v4l2_free(struct h264_v4l2 *e)
{
	int i;

	if (!e)
		return;
	if (e->fd >= 0) {
		enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;

		xioctl(e->fd, VIDIOC_STREAMOFF, &t);
		t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		xioctl(e->fd, VIDIOC_STREAMOFF, &t);
	}
	for (i = 0; i < NBUF; i++) {
		if (e->out[i].p)
			munmap(e->out[i].p, e->out[i].len);
		if (e->cap[i].p)
			munmap(e->cap[i].p, e->cap[i].len);
	}
	if (e->fd >= 0)
		close(e->fd);
	free(e->au);
	free(e);
}
