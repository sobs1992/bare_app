/*
 * System monitor (CTX_SYSMON): while it is shown (ACT_SYSMON, F5 held)
 * the readings of services/sysmon.c are taken every second. The first
 * sample comes at once with the loads unknown, the second soon after
 * with them. Hidden, nothing is read. SYSMON_FRAMES shows only the game's
 * frame times, which the view has itself: no readings then either.
 */
#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "core/util.h"
#include "gui/ctx.h"
#include "services/sysmon.h"

#define TAG "sysmon"
#define FIRST_MS 400		/* the second sample: loads over this long */
#define PERIOD_MS 1000

static struct {
	struct mainloop *ml;
	struct sysmon *mon;
	struct ml_timer *timer;
	unsigned seq;
	bool shown;
} sm;

static void take(void *unused)
{
	struct gui_ctx *c;
	struct sysmon_info info;
	uint64_t now = now_ms();

	(void)unused;
	sysmon_sample(sm.mon, now, &info);
	c = ctx_lock();
	c->sysmon.info = info;
	c->sysmon.taken_ms = now;
	c->sysmon.seq = ++sm.seq;
	ctx_unlock(CTX_SYSMON);
}

static void set_shown(bool shown, bool frames_only)
{
	struct gui_ctx *c = ctx_lock();

	c->sysmon.shown = shown;
	c->sysmon.frames_only = frames_only;
	if (!shown || frames_only)
		c->sysmon.seq = 0;
	ctx_unlock(CTX_SYSMON);
}

static void stop_readings(void)
{
	if (!sm.mon)
		return;
	mainloop_timer_cancel(sm.ml, sm.timer);
	sm.timer = NULL;
	sysmon_close(sm.mon);
	sm.mon = NULL;
}

static void hide(void)
{
	if (!sm.shown)
		return;
	stop_readings();
	sm.shown = false;
	set_shown(false, false);
	LOGI(TAG, "hidden");
}

static void show(void)
{
	if (sm.mon)
		return;
	sm.mon = sysmon_open(NULL);
	if (!sm.mon)
		return;
	sm.seq = 0;
	sm.shown = true;
	set_shown(true, false);
	take(NULL);
	sm.timer = mainloop_add_timer(sm.ml, FIRST_MS, PERIOD_MS, take, NULL);
	LOGI(TAG, "shown");
}

static void show_frames(void)
{
	stop_readings();
	sm.shown = true;
	set_shown(true, true);
	LOGI(TAG, "frame times only");
}

static int sysmon_init(const struct control_env *env)
{
	const char *at_start = getenv("BARE_APP_SYSMON");	/* full, frames */
	struct gui_ctx *c;

	sm.ml = env->ml;
	c = ctx_lock();
	c->sysmon.available = true;
	ctx_unlock(CTX_SYSMON);
	if (at_start && !strcmp(at_start, "full"))
		show();
	else if (at_start && !strcmp(at_start, "frames"))
		show_frames();
	return 0;
}

static void sysmon_action(const struct gui_action *a)
{
	if (a->type != ACT_SYSMON)
		return;
	switch (a->arg) {
	case SYSMON_FULL:
		show();
		break;
	case SYSMON_FRAMES:
		show_frames();
		break;
	default:
		hide();
		break;
	}
}

const struct control_ops ctl_sysmon = {
	.name = "sysmon",
	.init = sysmon_init,
	.on_action = sysmon_action,
	.deinit = hide,
};
