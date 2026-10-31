#include "view.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/mainloop.h"
#include "core/util.h"
#include "gui/action.h"
#include "hal/platform.h"
#include "media/mailbox.h"
#include "media/testpattern.h"

#define TAG "view"
#define MAX_SUBS 128

struct vsub {
	int id;			/* 0: free */
	uint32_t sections;
	view_cb cb;
	void *self;
};

static struct {
	const struct view_backend_ops *be;
	const struct platform *pf;
	pthread_t thread;
	sem_t ready;
	int status;
	struct mainloop *ml;
	struct hal_display *display;
	void *inputs[PLATFORM_MAX_INPUTS];
	struct gui_ctx snap;
	struct vsub subs[MAX_SUBS];
	int nsubs, next_id;
	view_grab_cb grab;
	void *grab_ud;
	bool quit_sent;
	enum video_source video_src;
	struct mailbox *video_mb;
	struct testpattern *video_tp;
	unsigned video_frames;
} v;

/* ---- subscriptions ---------------------------------------------------- */

int view_subscribe(uint32_t sections, view_cb cb, void *self)
{
	int i;

	for (i = 0; i < v.nsubs; i++)
		if (!v.subs[i].id)
			break;
	if (i == v.nsubs) {
		if (v.nsubs == MAX_SUBS) {
			LOGE(TAG, "too many subscriptions");
			return -1;
		}
		v.nsubs++;
	}
	v.subs[i] = (struct vsub){ ++v.next_id, sections, cb, self };
	cb(self, &v.snap, CTX_ALL);
	return v.subs[i].id;
}

void view_unsubscribe(int id)
{
	int i;

	for (i = 0; i < v.nsubs; i++)
		if (v.subs[i].id == id)
			v.subs[i].id = 0;
}

const struct gui_ctx *view_ctx(void)
{
	return &v.snap;
}

static void dispatch(uint32_t changed)
{
	int i, n = v.nsubs, id;

	/*
	 * Callbacks may subscribe (new screen) or unsubscribe (deleted
	 * screen). New entries are past 'n' or reuse freed slots with a newer
	 * id; both already got their first call from view_subscribe().
	 */
	id = v.next_id;
	for (i = 0; i < n; i++) {
		struct vsub *s = &v.subs[i];

		if (s->id && s->id <= id && (s->sections & changed))
			s->cb(s->self, &v.snap, changed & s->sections);
	}
}

/* ---- input ------------------------------------------------------------ */

void view_input_grab(view_grab_cb cb, void *ud)
{
	v.grab = cb;
	v.grab_ud = ud;
}

void view_input_release(void *ud)
{
	if (v.grab_ud == ud) {
		v.grab = NULL;
		v.grab_ud = NULL;
	}
}

static int button_to_key(enum gp_button b)
{
	switch (b) {
	case GP_UP:    return UI_KEY_UP;
	case GP_DOWN:  return UI_KEY_DOWN;
	case GP_LEFT:  return UI_KEY_LEFT;
	case GP_RIGHT: return UI_KEY_RIGHT;
	case GP_A:     return UI_KEY_ACCEPT;
	case GP_B:     return UI_KEY_BACK;
	case GP_L1:    return UI_KEY_PREV;
	case GP_R1:    return UI_KEY_NEXT;
	case GP_START: return UI_KEY_MENU;
	default:       return -1;
	}
}

static void on_gamepad(const struct gp_event *ev, void *ud)
{
	uint32_t combo = v.pf->quit_combo;
	int key;

	(void)ud;
	if (ev->type == GP_EV_BUTTON) {
		LOGD(TAG, "%s %s%s", gp_button_name(ev->code), ev->pressed ? "down" : "up",
		     ev->repeat ? " (repeat)" : "");
		if (combo && (input_buttons() & combo) == combo && !v.quit_sent) {
			v.quit_sent = true;
			gui_action_post(ACT_QUIT, 0);
			return;
		}
	}
	if (v.grab) {
		v.grab(ev, v.grab_ud);
		return;
	}
	if (ev->type != GP_EV_BUTTON)
		return;
	key = button_to_key(ev->code);
	if (key < 0)
		return;
	if (!v.be->on_key(key, ev->pressed, ev->repeat) && key == UI_KEY_BACK && ev->pressed &&
	    !ev->repeat)
		gui_action_post(ACT_BACK, 0);
}

const char *ui_key_name(enum ui_key key)
{
	static const char *const names[UI_KEY_COUNT] = {
		"up", "down", "left", "right", "accept", "back", "prev", "next", "menu",
	};

	return key < UI_KEY_COUNT ? names[key] : "?";
}

/* ---- video layer ------------------------------------------------------ */

static struct hal_layer *video_layer(void)
{
	return v.display ? hal_display_layer(v.display, LAYER_VIDEO) : NULL;
}

bool view_video_available(void)
{
	return video_layer() != NULL;
}

#ifdef CONFIG_VIDEO_TESTPATTERN
/* A new frame from the producer thread: copy it into the video layer */
static void on_video_frame(int fd, short revents, void *ud)
{
	struct hal_layer *l = video_layer();
	struct hal_fb *src, *dst;
	struct hal_rect all;
	int y;

	(void)fd;
	(void)revents;
	(void)ud;
	mailbox_ack(v.video_mb);
	src = mailbox_take(v.video_mb);
	if (!src || !l)
		return;
	dst = hal_layer_acquire(l);
	for (y = 0; y < dst->height && y < src->height; y++)
		memcpy((uint8_t *)dst->pixels + (size_t)y * dst->stride,
		       (uint8_t *)src->pixels + (size_t)y * src->stride,
		       (size_t)(dst->width < src->width ? dst->width : src->width) * 4);
	all = (struct hal_rect){ 0, 0, dst->width - 1, dst->height - 1 };
	hal_layer_damage(l, &all);
	hal_layer_submit(l);
	v.video_frames++;
}

static void video_stop(struct hal_layer *l)
{
	struct hal_fb *fb;
	struct hal_rect all;

	if (!v.video_mb)
		return;
	testpattern_stop(v.video_tp);
	mainloop_remove_fd(v.ml, mailbox_fd(v.video_mb));
	LOGI(TAG, "video: %u frames shown, %u dropped", v.video_frames, mailbox_dropped(v.video_mb));
	mailbox_free(v.video_mb);
	v.video_tp = NULL;
	v.video_mb = NULL;
	/* leave the layer black */
	fb = hal_layer_acquire(l);
	memset(fb->pixels, 0, (size_t)fb->stride * fb->height);
	all = (struct hal_rect){ 0, 0, fb->width - 1, fb->height - 1 };
	hal_layer_damage(l, &all);
	hal_layer_submit(l);
}

#endif /* CONFIG_VIDEO_TESTPATTERN */

int view_video_set(enum video_source src)
{
	struct hal_layer *l = video_layer();

	if (!l)
		return src == VIDEO_NONE ? 0 : -1;
	if (src == v.video_src)
		return 0;
#ifndef CONFIG_VIDEO_TESTPATTERN
	if (src != VIDEO_NONE)
		return -1;	/* not in this build */
#else
	video_stop(l);
	if (src == VIDEO_TEST_PATTERN) {
		struct hal_fb *fb = hal_layer_acquire(l);

		v.video_mb = mailbox_new(fb->width, fb->height);
		if (!v.video_mb)
			return -1;
		v.video_frames = 0;
		mainloop_add_fd(v.ml, mailbox_fd(v.video_mb), POLLIN, on_video_frame, NULL);
		v.video_tp = testpattern_start(v.video_mb, 30);
	}
#endif
	v.video_src = src;
	LOGI(TAG, "video source %d", src);
	return 0;
}

/* ---- loop ------------------------------------------------------------- */

static void on_ctx(int fd, short revents, void *ud)
{
	(void)fd;
	(void)revents;
	(void)ud;
	ctx_ack();	/* the snapshot is taken in prepare(), once per iteration */
}

/*
 * Screenshot of the composed layers as PPM: on SIGUSR1 (to BARE_APP_SHOT or
 * /tmp/bare_app.ppm), and on exit if BARE_APP_SHOT is set, taken before the
 * UI is torn down.
 */
static void snapshot(bool on_request)
{
#ifdef CONFIG_SNAPSHOT
	const char *path = getenv("BARE_APP_SHOT");

	if (!path || !*path) {
		if (!on_request)
			return;
		path = "/tmp/bare_app.ppm";
	}
	if (v.display->ops->snapshot)
		v.display->ops->snapshot(v.display, path);
	else
		LOGW(TAG, "display \"%s\" cannot take snapshots", v.display->ops->name);
#else
	(void)on_request;
#endif
}

static void on_notify(int fd, short revents, void *ud)
{
	struct gui_notify n;

	(void)fd;
	(void)revents;
	(void)ud;
	notify_ack();
	while (notify_pop(&n)) {
		if (n.kind == NOTIFY_SNAPSHOT)
			snapshot(true);
		else if (v.be->on_notify)
			v.be->on_notify(&n);
	}
}

static int prepare(void *ud)
{
	uint32_t changed;

	(void)ud;
	changed = ctx_snapshot(&v.snap);
	if (changed)
		dispatch(changed);
	return v.be->process ? v.be->process() : -1;
}

static void close_devices(void)
{
	int i;

	for (i = 0; i < PLATFORM_MAX_INPUTS; i++)
		if (v.inputs[i]) {
			((const struct hal_input_ops *)v.pf->inputs[i].ops)->close(v.inputs[i]);
			v.inputs[i] = NULL;
		}
	if (v.display) {
		v.display->ops->close(v.display);
		v.display = NULL;
	}
}

static int open_devices(void)
{
	int i, n = 0;

	for (i = 0; i < PLATFORM_MAX_DISPLAYS && !v.display; i++) {
		const struct hal_display_ops *dops = v.pf->displays[i].ops;

		if (!dops)
			continue;
		v.display = dops->open(v.pf->displays[i].params, v.ml);
		if (!v.display)
			LOGW(TAG, "display \"%s\" did not open", dops->name);
		else
			LOGI(TAG, "display \"%s\"", dops->name);
	}
	if (!v.display) {
		LOGE(TAG, "no display");
		return -1;
	}
	for (i = 0; i < PLATFORM_MAX_INPUTS; i++) {
		const struct hal_input_ops *iops = v.pf->inputs[i].ops;

		if (!iops)
			continue;
		v.inputs[i] = iops->open(v.pf->inputs[i].params, v.ml);
		n += v.inputs[i] != NULL;
	}
	if (!n)
		LOGW(TAG, "no input device opened");
	return 0;
}

static void *view_thread(void *arg)
{
	(void)arg;
	v.ml = mainloop_new("view");
	if (!v.ml) {
		v.status = -1;
		sem_post(&v.ready);
		return NULL;
	}
	input_init(v.ml, NULL, on_gamepad, NULL);
	v.status = open_devices();
	if (!v.status) {
		ctx_snapshot(&v.snap);
		v.status = v.be->init(v.display, v.ml);
		if (v.status)
			LOGE(TAG, "backend \"%s\" failed to start", v.be->name);
	}
	if (!v.status) {
		mainloop_add_fd(v.ml, ctx_fd(), POLLIN, on_ctx, NULL);
		mainloop_add_fd(v.ml, notify_fd(), POLLIN, on_notify, NULL);
		mainloop_set_prepare(v.ml, prepare, NULL);
	}
	sem_post(&v.ready);
	if (!v.status) {
		LOGI(TAG, "running, backend \"%s\"", v.be->name);
		mainloop_run(v.ml);
		snapshot(false);
		v.be->deinit();
		view_video_set(VIDEO_NONE);
	}
	close_devices();
	input_deinit();
	mainloop_free(v.ml);
	v.ml = NULL;
	return NULL;
}

int view_start(const struct view_backend_ops *be, const struct platform *pf)
{
	int err;

	memset(&v, 0, sizeof(v));
	v.be = be;
	v.pf = pf;
	sem_init(&v.ready, 0, 0);
	err = pthread_create(&v.thread, NULL, view_thread, NULL);
	if (err) {
		LOGE(TAG, "pthread_create: %s", strerror(err));
		return -1;
	}
	while (sem_wait(&v.ready) < 0 && errno == EINTR)
		;
	if (v.status) {
		pthread_join(v.thread, NULL);
		return -1;
	}
	return 0;
}

void view_stop(void)
{
	if (!v.ml)
		return;
	mainloop_quit(v.ml);
	pthread_join(v.thread, NULL);
	sem_destroy(&v.ready);
}
