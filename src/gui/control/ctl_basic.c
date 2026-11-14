/* Small control modules: application lifetime, navigation, system info. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "control.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "gui/ctx.h"
#include "hal/platform.h"

/* ---- app: quitting, powering off -------------------------------------- */

static struct mainloop *app_loop;
static const char *app_poweroff_avail, *app_poweroff;

static int app_init(const struct control_env *env)
{
	app_loop = env->ml;
	app_poweroff_avail = env->pf->poweroff;
	return 0;
}

static void app_action(const struct gui_action *a)
{
	if (a->type == ACT_POWER_OFF && app_poweroff_avail) {
		LOGI("app", "power off requested");
		app_poweroff = app_poweroff_avail;
		mainloop_quit(app_loop);
	} else if (a->type == ACT_QUIT || a->type == ACT_POWER_OFF) {
		LOGI("app", "quit requested");
		mainloop_quit(app_loop);
	}
}

/* main() runs it after the screen has gone back to the console */
const char *control_poweroff_cmd(void)
{
	return app_poweroff;
}

const struct control_ops ctl_app = {
	.name = "app",
	.init = app_init,
	.on_action = app_action,
};

/* ---- nav: screen stack ------------------------------------------------ */

static int nav_init(const struct control_env *env)
{
	struct gui_ctx *c = ctx_lock();

	(void)env;
	c->nav.stack[0] = SCREEN_HOME;
	c->nav.depth = 1;
	ctx_unlock(CTX_NAV);
	return 0;
}

static void nav_action(const struct gui_action *a)
{
	struct gui_ctx *c;
	uint32_t changed = 0;

	if (a->type != ACT_OPEN_SCREEN && a->type != ACT_BACK)
		return;
	c = ctx_lock();
	if (a->type == ACT_OPEN_SCREEN) {
		if (a->arg >= 0 && a->arg < SCREEN_COUNT && c->nav.depth < NAV_DEPTH &&
		    c->nav.stack[c->nav.depth - 1] != (enum screen_id)a->arg) {
			c->nav.stack[c->nav.depth++] = a->arg;
			changed = CTX_NAV;
		}
	} else if (c->nav.depth > 1) {
		c->nav.depth--;
		changed = CTX_NAV;
	}
	if (changed)
		LOGD("nav", "screen %d, depth %d", c->nav.stack[c->nav.depth - 1], c->nav.depth);
	ctx_unlock(changed);
}

const struct control_ops ctl_nav = {
	.name = "nav",
	.init = nav_init,
	.on_action = nav_action,
};

/* ---- system: what we run on, its clock --------------------------------- */

static const char *clock_cmd;

static int system_init(const struct control_env *env)
{
	struct gui_ctx *c = ctx_lock();

	snprintf(c->system.platform, sizeof(c->system.platform), "%s", env->pf->name);
	/* "" when unknown: wording is the view's business */
	snprintf(c->system.model, sizeof(c->system.model), "%s", platform_model());
	c->system.can_hibernate = env->pf->hibernate != NULL;
	c->system.can_set_clock = env->pf->clock != NULL;
	clock_cmd = env->pf->clock;
	ctx_unlock(CTX_SYSTEM);
	return 0;
}

/* "<clock> set 'YYYY-MM-DD hh:mm'": the system time and the RTC */
static void set_clock(const char *when)
{
	int y, mo, d, h, mi, n = 0, status;
	pid_t pid;

	if (!clock_cmd)
		return;
	if (sscanf(when, "%4d-%2d-%2d %2d:%2d%n", &y, &mo, &d, &h, &mi, &n) != 5 ||
	    when[n] || y < 2000 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59) {
		LOGW("system", "bad date and time \"%s\"", when);
		return;
	}
	pid = fork();
	if (pid < 0) {
		LOGW("system", "fork: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		execl(clock_cmd, clock_cmd, "set", when, (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return;
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		LOGW("system", "%s set failed (status %#x)", clock_cmd, status);
	else
		LOGI("system", "date and time set: %s", when);
}

static void system_action(const struct gui_action *a)
{
	struct gui_ctx *c;

	if (a->type == ACT_SET_CLOCK) {
		set_clock(a->str);
	} else if (a->type == ACT_INPUT_DEVICES) {
		c = ctx_lock();
		c->input.devices = (uint32_t)a->arg;
		ctx_unlock(CTX_INPUT);
	}
}

const struct control_ops ctl_system = {
	.name = "system",
	.init = system_init,
	.on_action = system_action,
};
