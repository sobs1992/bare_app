/*
 * Power: battery state into CTX_POWER, low battery and charger as
 * notifications; sleep (ACT_SUSPEND) through the platform's command.
 */
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "control.h"
#include "core/bus.h"
#include "core/log.h"
#include "gui/ctx.h"
#include "gui/notify.h"
#include "hal/platform.h"
#include "services/services.h"

#define TAG "power"

#define LOW_BATTERY 15

static int sub;
static const char *suspend_cmd;
static struct hal_power_state prev;
static bool have_prev;

static void on_power(const struct ev *e, void *ud)
{
	const struct hal_power_state *p = &e->u.power;
	struct gui_ctx *c = ctx_lock();

	(void)ud;
	c->power.present = p->present;
	c->power.capacity = p->capacity;
	c->power.charging = p->charging;
	c->power.full = p->full;
	c->power.online = p->online;
	ctx_unlock(CTX_POWER);

	if (have_prev) {
		if (p->online != prev.online)
			gui_notify_post(NOTIFY_CHARGER, p->online);
		if (!p->online && p->capacity >= 0 && p->capacity <= LOW_BATTERY &&
		    (prev.capacity > LOW_BATTERY || prev.online))
			gui_notify_post(NOTIFY_BATTERY_LOW, p->capacity);
	}
	prev = *p;
	have_prev = true;
}

static int power_init(const struct control_env *env)
{
	struct gui_ctx *c = ctx_lock();

	suspend_cmd = env->pf->suspend;
	c->power.available = power_available();
	ctx_unlock(CTX_POWER);
	sub = bus_subscribe(EV_POWER_CHANGED, on_power, NULL);
	return sub > 0 ? 0 : -1;
}

/* The command returns once the device has woken up; the control thread
 * waits for it, nothing else happens meanwhile anyway */
static int run_suspend(void)
{
	int status;
	pid_t pid = fork();

	if (pid < 0) {
		LOGW(TAG, "fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execl(suspend_cmd, suspend_cmd, (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		LOGW(TAG, "%s failed (status %#x)", suspend_cmd, status);
		return -1;
	}
	return 0;
}

static void power_action(const struct gui_action *a)
{
	int ret = -1;

	if (a->type != ACT_SUSPEND)
		return;
	if (suspend_cmd) {
		LOGI(TAG, "going to sleep: %s", suspend_cmd);
		ret = run_suspend();
		LOGI(TAG, ret == 0 ? "woken up" : "did not sleep");
	}
	gui_notify_post(NOTIFY_RESUMED, ret);
}

static void power_deinit(void)
{
	bus_unsubscribe(sub);
}

const struct control_ops ctl_power = {
	.name = "power",
	.init = power_init,
	.on_action = power_action,
	.deinit = power_deinit,
};
