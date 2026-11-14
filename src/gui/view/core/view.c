/*
 * The view core's thread (view.h, the parts: view_priv.h): it opens the
 * display and the input devices, runs the loop, takes the context's
 * snapshots for the subscriptions, passes notifications on and takes
 * snapshots of the screen.
 */
#include "view_priv.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "core/mainloop.h"
#include "core/util.h"
#include "gui/action.h"
#include "media/shot.h"

#define TAG "view"

struct view_state vcore;

/* ---- subscriptions ---------------------------------------------------- */

int view_subscribe(uint32_t sections, view_cb cb, void *self)
{
	int i;

	for (i = 0; i < vcore.nsubs; i++)
		if (!vcore.subs[i].id)
			break;
	if (i == vcore.nsubs) {
		if (vcore.nsubs == MAX_SUBS) {
			LOGE(TAG, "too many subscriptions");
			return -1;
		}
		vcore.nsubs++;
	}
	vcore.subs[i] = (struct vsub){ ++vcore.next_id, sections, cb, self };
	cb(self, &vcore.snap, CTX_ALL);
	return vcore.subs[i].id;
}

void view_unsubscribe(int id)
{
	int i;

	for (i = 0; i < vcore.nsubs; i++)
		if (vcore.subs[i].id == id)
			vcore.subs[i].id = 0;
}

const struct gui_ctx *view_ctx(void)
{
	return &vcore.snap;
}

static void dispatch(uint32_t changed)
{
	int i, n = vcore.nsubs, id;

	/*
	 * Callbacks may subscribe (new screen) or unsubscribe (deleted
	 * screen). New entries are past 'n' or reuse freed slots with a newer
	 * id; both already got their first call from view_subscribe().
	 */
	id = vcore.next_id;
	for (i = 0; i < n; i++) {
		struct vsub *s = &vcore.subs[i];

		if (s->id && s->id <= id && (s->sections & changed))
			s->cb(s->self, &vcore.snap, changed & s->sections);
	}
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
	if (vcore.display->ops->layers) {
		const struct hal_fb *ui, *video;

		vcore.display->ops->layers(vcore.display, &ui, &video);
		shot_write_ppm(path, ui, video);
	} else
		LOGW(TAG, "display \"%s\" cannot take snapshots", vcore.display->ops->name);
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
		if (n.kind == NOTIFY_SNAPSHOT) {
			snapshot(true);
		} else if (n.kind == NOTIFY_RESUMED) {
			vcore.asleep = false;
			vcore.power_quiet_until = now_ms() + POWER_QUIET_MS;
			view_screen_on();	/* also if the device could not sleep */
		} else if (vcore.be->on_notify) {
			vcore.be->on_notify(&n);
		}
	}
}

static int prepare(void *ud)
{
	uint32_t changed;

	(void)ud;
	changed = ctx_snapshot(&vcore.snap);
	if (changed & CTX_SETTINGS)
		view_stream_follow();
	if (changed)
		dispatch(changed);
	return vcore.be->process ? vcore.be->process() : -1;
}

static void close_devices(void)
{
	int i;

	for (i = 0; i < PLATFORM_MAX_INPUTS; i++)
		if (vcore.inputs[i]) {
			((const struct hal_input_ops *)vcore.pf->inputs[i].ops)->close(vcore.inputs[i]);
			vcore.inputs[i] = NULL;
		}
	if (vcore.display) {
		vcore.display->ops->close(vcore.display);
		vcore.display = NULL;
	}
}

static int open_devices(void)
{
	int i, n = 0;

	for (i = 0; i < PLATFORM_MAX_DISPLAYS && !vcore.display; i++) {
		const struct hal_display_ops *dops = vcore.pf->displays[i].ops;

		if (!dops)
			continue;
		vcore.display = dops->open(vcore.pf->displays[i].params, vcore.ml);
		if (!vcore.display)
			LOGW(TAG, "display \"%s\" did not open", dops->name);
		else
			LOGI(TAG, "display \"%s\"", dops->name);
	}
	if (!vcore.display) {
		LOGE(TAG, "no display");
		return -1;
	}
	for (i = 0; i < PLATFORM_MAX_INPUTS; i++) {
		const struct hal_input_ops *iops = vcore.pf->inputs[i].ops;

		if (!iops)
			continue;
		vcore.inputs[i] = iops->open(vcore.pf->inputs[i].params, vcore.ml);
		n += vcore.inputs[i] != NULL;
	}
	if (!n)
		LOGW(TAG, "no input device opened");
	return 0;
}

/* Next to the settings file: bare_app_sticks.conf (the input test writes it) */
static void load_stick_calibration(void)
{
#ifdef CONFIG_SETTINGS_FILE
	const char *slash = vcore.pf->config_path ? strrchr(vcore.pf->config_path, '/') : NULL;
	char path[256];

	if (!slash)
		return;
	snprintf(path, sizeof(path), "%.*s/bare_app_sticks.conf",
		 (int)(slash - vcore.pf->config_path), vcore.pf->config_path);
	input_load_calibration(path);
#endif
}

/* Keyboards, mice, the motion sensor come and go: to the context (status bar) */
static void on_devices(uint32_t devices)
{
	gui_action_post(ACT_INPUT_DEVICES, (int)devices);
}

static void *view_thread(void *arg)
{
	(void)arg;
	vcore.ml = mainloop_new("view");
	if (!vcore.ml) {
		vcore.status = -1;
		sem_post(&vcore.ready);
		return NULL;
	}
	input_init(vcore.ml, NULL, view_on_gamepad, NULL);
	input_set_raw_query(view_hid_game_in_front);
	input_set_devices_cb(on_devices);
	load_stick_calibration();
	vcore.status = open_devices();
	if (!vcore.status) {
		ctx_snapshot(&vcore.snap);
		vcore.status = vcore.be->init(vcore.display, vcore.ml);
		if (vcore.status)
			LOGE(TAG, "backend \"%s\" failed to start", vcore.be->name);
	}
	if (!vcore.status) {
		/* the first snapshot took the settings in without a change to
		 * follow: a stream switched on before starts here */
		view_stream_follow();
		mainloop_add_fd(vcore.ml, ctx_fd(), POLLIN, on_ctx, NULL);
		mainloop_add_fd(vcore.ml, notify_fd(), POLLIN, on_notify, NULL);
		mainloop_set_prepare(vcore.ml, prepare, NULL);
	}
	sem_post(&vcore.ready);
	if (!vcore.status) {
		LOGI(TAG, "running, backend \"%s\"", vcore.be->name);
		mainloop_run(vcore.ml);
		snapshot(false);
		vcore.be->deinit();
		view_video_set(VIDEO_NONE);
		view_stream_end();
	}
	close_devices();
	input_deinit();
	mainloop_free(vcore.ml);
	vcore.ml = NULL;
	return NULL;
}

int view_start(const struct view_backend_ops *be, const struct platform *pf)
{
	int err;

	memset(&vcore, 0, sizeof(vcore));
	vcore.be = be;
	vcore.pf = pf;
	sem_init(&vcore.ready, 0, 0);
	err = pthread_create(&vcore.thread, NULL, view_thread, NULL);
	if (err) {
		LOGE(TAG, "pthread_create: %s", strerror(err));
		return -1;
	}
	while (sem_wait(&vcore.ready) < 0 && errno == EINTR)
		;
	if (vcore.status) {
		pthread_join(vcore.thread, NULL);
		return -1;
	}
	return 0;
}

void view_stop(void)
{
	if (!vcore.ml)
		return;
	mainloop_quit(vcore.ml);
	pthread_join(vcore.thread, NULL);
	sem_destroy(&vcore.ready);
}
