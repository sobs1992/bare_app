/*
 * The view core's video layer (view_priv.h): its sources (the test
 * pattern, games in their own processes), the times frames were shown,
 * and the stream of the screen.
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
#include "i18n.h"
#include "media/mailbox.h"
#include "media/shot.h"
#include "media/testpattern.h"
#ifdef CONFIG_STREAM
#include "stream/stream.h"
#endif

#define TAG "view"

static const char *game_pcm(void);

/* ---- video layer ------------------------------------------------------ */

static struct hal_layer *video_layer(void)
{
	return vcore.display ? hal_display_layer(vcore.display, LAYER_VIDEO) : NULL;
}

bool view_video_available(void)
{
	return video_layer() != NULL;
}

#ifdef CONFIG_GAMES
#define HAVE_GAMES
#endif
#if defined(CONFIG_VIDEO_TESTPATTERN) || defined(HAVE_GAMES)
#define HAVE_VIDEO_SOURCES
#endif

#ifdef HAVE_VIDEO_SOURCES
#define FRAME_STATS_PERIOD_US 10000000
#define FRAME_LATE_US 60000

/* Frame times start again: a new source, or one back from the background */
static void frames_restart(bool new_source)
{
	frametimes_reset(&vcore.frame_times);
	vcore.frame_stats.last_us = 0;
	if (!new_source)
		return;
	vcore.frame_stats.over50 = vcore.frame_stats.over100 = 0;
	vcore.frame_stats.worst_us = 0;
	vcore.frame_stats.log_us = getenv("BARE_APP_FRAME_STATS") ? now_us() : 0;
	vcore.frame_stats.log_late = vcore.frame_stats.log_us && !strcmp(getenv("BARE_APP_FRAME_STATS"), "2");
}

/* BARE_APP_FRAME_STATS: the last stretch in the log */
static void frames_log(uint64_t now)
{
	static uint64_t t[FRAMETIMES_MAX];
	struct frametimes_summary s;
	unsigned n = frametimes_last(&vcore.frame_times, t, FRAMETIMES_MAX);

	frametimes_summarize(t, n, vcore.frame_stats.log_us, 0, &s);
	vcore.frame_stats.log_us = now;
	if (!s.frames)
		return;
	LOGI(TAG, "frames: %u in %llu ms, %d.%d fps, median %u.%u ms, 99%% %u.%u ms, worst %u.%u ms, "
	     "%u hitches", s.frames, (unsigned long long)(s.span_us / 1000), s.fps_x10 / 10,
	     s.fps_x10 % 10, s.median_us / 1000, s.median_us / 100 % 10, s.p99_us / 1000,
	     s.p99_us / 100 % 10, s.worst_us / 1000, s.worst_us / 100 % 10, s.hitches);
}

static void frame_shown(void)
{
	uint64_t now = now_us();
	uint32_t d = vcore.frame_stats.last_us ? (uint32_t)MIN(now - vcore.frame_stats.last_us, UINT32_MAX) : 0;

	frametimes_add(&vcore.frame_times, now);
	vcore.frame_stats.last_us = now;
	vcore.frame_stats.over50 += d > 50000;
	vcore.frame_stats.over100 += d > 100000;
	vcore.frame_stats.worst_us = MAX(vcore.frame_stats.worst_us, d);
	if (vcore.frame_stats.log_late && d > FRAME_LATE_US)
		LOGI(TAG, "frame late: %u ms, shown at %llu us", d / 1000, (unsigned long long)now);
	if (vcore.frame_stats.log_us && now - vcore.frame_stats.log_us >= FRAME_STATS_PERIOD_US)
		frames_log(now);
}

static void frames_end_log(const char *what)
{
	LOGI(TAG, "%s: %u frames shown, %u over 50 ms, %u over 100 ms, worst %u ms", what,
	     vcore.video_frames, vcore.frame_stats.over50, vcore.frame_stats.over100,
	     vcore.frame_stats.worst_us / 1000);
}

/* Copy a whole frame of a source into the video layer and show it */
static void video_show(struct hal_layer *l, const struct hal_fb *src)
{
	struct hal_fb *dst = hal_layer_acquire(l);
	struct hal_rect all;
	int y;

	for (y = 0; y < dst->height && y < src->height; y++)
		memcpy((uint8_t *)dst->pixels + (size_t)y * dst->stride,
		       (uint8_t *)src->pixels + (size_t)y * src->stride,
		       (size_t)(dst->width < src->width ? dst->width : src->width) * 4);
	all = (struct hal_rect){ 0, 0, dst->width - 1, dst->height - 1 };
	hal_layer_damage(l, &all);
	hal_layer_submit(l);
	vcore.video_frames++;
	frame_shown();
}

static void video_clear(struct hal_layer *l)
{
	struct hal_fb *fb = hal_layer_acquire(l);
	struct hal_rect all;

	memset(fb->pixels, 0, (size_t)fb->stride * fb->height);
	all = (struct hal_rect){ 0, 0, fb->width - 1, fb->height - 1 };
	hal_layer_damage(l, &all);
	hal_layer_submit(l);
}
#endif

#ifdef CONFIG_VIDEO_TESTPATTERN
/* A new frame from the producer thread: copy it into the video layer */
static void on_video_frame(int fd, short revents, void *ud)
{
	struct hal_layer *l = video_layer();
	struct hal_fb *src;

	(void)fd;
	(void)revents;
	(void)ud;
	mailbox_ack(vcore.video_mb);
	src = mailbox_take(vcore.video_mb);
	if (src && l)
		video_show(l, src);
}
#endif

#ifdef HAVE_GAMES
/* A frame from the game, or the game has ended */
static void on_game(int fd, short revents, void *ud)
{
	struct hal_layer *l = video_layer();
	const struct hal_fb *fb;
	bool ended, same, minimized = vcore.video_minimized;
	int status;

	(void)revents;
	(void)ud;
	fb = game_proc_frame(vcore.video_game, &ended, &same);
	if (fb && l && !minimized)
		video_show(l, fb);
	else if (same && l && !minimized && vcore.video_frames)
		frame_shown();	/* the picture stays, the frame counts */
	if (!ended)
		return;
	mainloop_remove_fd(vcore.ml, fd);
	status = game_proc_stop(vcore.video_game);
	vcore.video_game = NULL;
	vcore.video_minimized = false;
	frames_end_log(vcore.game.def->id);
	vcore.game = (struct game_ref){ 0 };
	if (l)
		video_clear(l);
	vcore.video_src = VIDEO_NONE;
	/* a minimised game has no screen that would listen */
	if (vcore.video_end && !minimized)
		vcore.video_end(status, vcore.video_end_ud);
}
#endif

/* A game that takes the gamepad now: the quit combo ends it */
bool view_game_in_front(void)
{
#ifdef HAVE_GAMES
	return vcore.video_game && !vcore.video_minimized;
#else
	return false;
#endif
}

/* core/input: keys or the mouse go to the game as they are */
bool view_hid_game_in_front(bool mouse)
{
#ifdef HAVE_GAMES
	return view_game_in_front() && game_proc_hid(vcore.video_game, mouse);
#else
	(void)mouse;
	return false;
#endif
}

/* The quit combo held over a game: as if it was quit in its menu */
void view_game_quit(void)
{
	view_video_end_cb cb = vcore.video_end;
	void *ud = vcore.video_end_ud;

	if (!view_game_in_front())
		return;
	LOGI(TAG, "quit combo: ending the game");
	view_video_set(VIDEO_NONE);
	if (cb)
		cb(0, ud);	/* exit status 0 */
}

/* F5 outside a game: back to the minimised one */
void view_game_unminimize(void)
{
	if (view_video_minimized() != VIDEO_GAME)
		return;
	vcore.game_sel = vcore.game;
	gui_action_post(ACT_OPEN_SCREEN, SCREEN_GAME);
}

#ifdef HAVE_VIDEO_SOURCES
static void video_stop(struct hal_layer *l)
{
	if (vcore.video_src == VIDEO_NONE)
		return;
#ifdef CONFIG_VIDEO_TESTPATTERN
	if (vcore.video_mb) {
		testpattern_stop(vcore.video_tp);
		mainloop_remove_fd(vcore.ml, mailbox_fd(vcore.video_mb));
		LOGI(TAG, "video: %u frames shown, %u dropped", vcore.video_frames,
		     mailbox_dropped(vcore.video_mb));
		mailbox_free(vcore.video_mb);
		vcore.video_tp = NULL;
		vcore.video_mb = NULL;
	}
#endif
#ifdef HAVE_GAMES
	if (vcore.video_game) {
		mainloop_remove_fd(vcore.ml, game_proc_fd(vcore.video_game));
		game_proc_stop(vcore.video_game);
		vcore.video_game = NULL;
		vcore.video_minimized = false;
		frames_end_log(vcore.game.def->id);
		vcore.game = (struct game_ref){ 0 };
	}
#endif
	video_clear(l);	/* leave the layer black */
}
#endif

void view_video_input(const struct gp_event *ev)
{
#ifdef HAVE_GAMES
	if (vcore.video_game)
		game_proc_input(vcore.video_game, ev);
#else
	(void)ev;
#endif
}

void view_video_minimize(void)
{
#ifdef HAVE_GAMES
	if (!view_game_in_front())
		return;
	game_proc_pause(vcore.video_game);
	vcore.video_minimized = true;
	LOGI(TAG, "%s minimised", vcore.game.def->id);
#endif
}

enum gp_button view_game_minimize_button(void)
{
#ifdef HAVE_GAMES
	return game_minimize_button(vcore.game, vcore.pf);
#else
	return GP_F5;
#endif
}

void view_game_select(struct game_ref g)
{
	vcore.game_sel = g;
}

struct game_ref view_game_selected(void)
{
	return vcore.game_sel;
}

struct game_ref view_game(void)
{
	return vcore.game;
}

const struct platform *view_platform(void)
{
	return vcore.pf;
}

enum video_source view_video_shown(void)
{
	return view_video_minimized() == VIDEO_NONE ? vcore.video_src : VIDEO_NONE;
}

enum video_source view_video_minimized(void)
{
#ifdef HAVE_GAMES
	return vcore.video_minimized ? vcore.video_src : VIDEO_NONE;
#else
	return VIDEO_NONE;
#endif
}

unsigned view_video_frames(void)
{
	return vcore.video_frames;
}

unsigned view_video_frame_times(uint64_t *out, unsigned max)
{
	return frametimes_last(&vcore.frame_times, out, max);
}

bool view_video_paced(void)
{
#ifdef HAVE_GAMES
	if (vcore.video_src == VIDEO_GAME)
		return vcore.video_game && (vcore.game.def->every_frame || game_proc_paced(vcore.video_game));
#endif
	return vcore.video_src != VIDEO_NONE;	/* the test pattern makes every frame */
}

void view_video_on_end(view_video_end_cb cb, void *ud)
{
	vcore.video_end = cb;
	vcore.video_end_ud = ud;
}

int view_video_set(enum video_source src)
{
	struct hal_layer *l = video_layer();
	struct hal_fb *fb;

	if (!l) {
		errno = ENODEV;
		return src == VIDEO_NONE ? 0 : -1;
	}
	if (src == vcore.video_src && (src != VIDEO_GAME || game_ref_eq(vcore.game, vcore.game_sel))) {
#ifdef HAVE_GAMES
		if (vcore.video_minimized) {
			/* its last frame is still in the layer, the count goes on */
			game_proc_resume(vcore.video_game);
			vcore.video_minimized = false;
			frames_restart(false);	/* the time away is no hitch */
			LOGI(TAG, "%s back", vcore.game.def->id);
		}
#endif
		return 0;
	}
#ifdef HAVE_VIDEO_SOURCES
	video_stop(l);
#endif
	vcore.video_src = VIDEO_NONE;
	vcore.video_frames = 0;
#ifdef HAVE_VIDEO_SOURCES
	frames_restart(true);
#endif
	switch (src) {
	case VIDEO_NONE:
		break;
#ifdef CONFIG_VIDEO_TESTPATTERN
	case VIDEO_TEST_PATTERN:
		fb = hal_layer_acquire(l);
		vcore.video_mb = mailbox_new(fb->width, fb->height);
		if (!vcore.video_mb)
			return -1;
		mainloop_add_fd(vcore.ml, mailbox_fd(vcore.video_mb), POLLIN, on_video_frame, NULL);
		vcore.video_tp = testpattern_start(vcore.video_mb, 30);
		break;
#endif
#ifdef HAVE_GAMES
	case VIDEO_GAME: {
		struct game_env env = { .pf = vcore.pf, .pcm = game_pcm(), .lang = i18n_lang() };

		if (!vcore.game_sel.def) {
			errno = EINVAL;
			return -1;
		}
		fb = hal_layer_acquire(l);
		env.width = fb->width;
		env.height = fb->height;
		vcore.video_game = vcore.game_sel.def->start(vcore.game_sel.entry, &env);
		if (!vcore.video_game)
			return -1;
		vcore.game = vcore.game_sel;
		mainloop_add_fd(vcore.ml, game_proc_fd(vcore.video_game), POLLIN, on_game, NULL);
		LOGI(TAG, "game %s, entry %d", vcore.game.def->id, vcore.game.entry);
		break;
	}
#endif
	default:
		(void)fb;
		errno = ENOTSUP;	/* not in this build */
		return -1;
	}
	vcore.video_src = src;
	LOGI(TAG, "video source %d", src);
	return 0;
}

/* ---- the stream (stream/stream.h) ------------------------------------- */

#ifdef CONFIG_STREAM
#define STREAM_FPS 30
#define STREAM_KBPS 3000
#define STREAM_PORT 8554

/* The game's own picture (game_ipc.h) can be bigger than the screen */
#define STREAM_MAX_WIDTH 1280
#define STREAM_MAX_HEIGHT 960

/* The picture of a game in front as it drew it, before it was taken down to
 * the screen, if it gives one (VCMI); NULL otherwise */
static const struct hal_fb *game_full(bool watched)
{
#ifdef HAVE_GAMES
	if (!vcore.video_game)
		return NULL;
	game_proc_want_full(vcore.video_game, watched && view_game_in_front());
	return watched && view_game_in_front() ? game_proc_full(vcore.video_game) : NULL;
#else
	(void)watched;
	return NULL;
#endif
}

/* The screen as it is into the stream's frame, while somebody watches; a
 * game in front as it drew it when it says (bare_app's own overlays, as
 * the frame rate, are not in it then) */
static void on_stream_tick(void *ud)
{
	const struct hal_fb *ui, *video, *full = game_full(stream_viewers() > 0);
	struct hal_fb *fb;
	int y;

	(void)ud;
	if (full) {
		if (!(fb = stream_frame_begin(full->width, full->height)))
			return;
		for (y = 0; y < fb->height; y++)
			memcpy((uint8_t *)fb->pixels + (size_t)y * fb->stride,
			       (const uint8_t *)full->pixels + (size_t)y * full->stride, (size_t)fb->width * 4);
		stream_frame_end();
		return;
	}
	if (!vcore.display->ops->layers || !stream_viewers())
		return;
	vcore.display->ops->layers(vcore.display, &ui, &video);
	if (!(fb = stream_frame_begin(ui->width, ui->height)))
		return;
	shot_compose(fb, ui, video);
	stream_frame_end();
}

/* The setting says it runs or not: start or stop it */
void view_stream_follow(void)
{
	struct hal_display_info di;
	struct stream_config c = {
		.max_width = STREAM_MAX_WIDTH, .max_height = STREAM_MAX_HEIGHT,
		.fps = STREAM_FPS, .kbps = STREAM_KBPS, .port = STREAM_PORT,
		.playback = vcore.pf->game_pcm,
	};

	if (vcore.snap.settings.stream == stream_running())
		return;
	if (!vcore.snap.settings.stream) {
		mainloop_timer_cancel(vcore.ml, vcore.stream_tick);
		vcore.stream_tick = NULL;
		stream_stop();
		vcore.stream_play[0] = vcore.stream_cap[0] = '\0';
		return;
	}
	hal_display_info(vcore.display, &di);
	c.width = di.width;
	c.height = di.height;
	/* games started from now on play into the loopback: the stream takes
	 * their sound from there and passes it on to the device */
	if (vcore.pf->game_pcm &&
	    stream_loopback(vcore.stream_play, vcore.stream_cap, sizeof(vcore.stream_play)) == 0)
		c.capture = vcore.stream_cap;
	else
		vcore.stream_play[0] = vcore.stream_cap[0] = '\0';
	if (stream_start(&c) < 0) {
		vcore.stream_play[0] = vcore.stream_cap[0] = '\0';
		return;
	}
	vcore.stream_tick = mainloop_add_timer(vcore.ml, 1000 / STREAM_FPS, 1000 / STREAM_FPS, on_stream_tick,
					   NULL);
}
#endif

void view_stream_end(void)
{
#ifdef CONFIG_STREAM
	if (vcore.stream_tick)
		mainloop_timer_cancel(vcore.ml, vcore.stream_tick);
	vcore.stream_tick = NULL;
	stream_stop();
#endif
}

#ifndef CONFIG_STREAM
void view_stream_follow(void)
{
}
#endif

/* The ALSA device games play to: the stream's loopback while it runs */
static const char *game_pcm(void)
{
#ifdef CONFIG_STREAM
	if (stream_running() && vcore.stream_play[0])
		return vcore.stream_play;
#endif
	return vcore.pf->game_pcm;
}
