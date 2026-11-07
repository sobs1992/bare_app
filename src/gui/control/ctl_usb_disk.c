/*
 * USB disk: the games partition for the computer, over the USB-C cable,
 * while SCREEN_USB_DISK is open (CTX_USB_DISK). Done by the platform's
 * command, "<cmd> start|status|stop" (the SDK's /usr/sbin/usb-disk);
 * BARE_APP_USB_DISK overrides it (tests on the PC).
 *
 * Opening the screen shares the partition, closing it takes it back. So
 * does the computer ejecting the disk: "status" sees that every second.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "control.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "gui/ctx.h"
#include "hal/platform.h"

#define TAG "usb-disk"

#define POLL_MS 1000
/* A game closed just before (from the home menu) may still hold /games */
#define BUSY_TRIES 10
#define BUSY_WAIT_US 300000

/* exit codes of the command */
enum { CMD_OK = 0, CMD_NOT_SHARED = 1, CMD_BUSY = 2, CMD_EJECTED = 3, CMD_NO_HOST = 4 };

static struct {
	struct mainloop *ml;
	const char *cmd;
	bool shared;
	struct ml_timer *poll;
} ud;

/* The command's exit code, -1 if it did not run */
static int run(const char *what)
{
	int status;
	pid_t pid = fork();

	if (pid < 0) {
		LOGW(TAG, "fork: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execl(ud.cmd, ud.cmd, what, (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) == 127)
		return -1;
	return WEXITSTATUS(status);
}

static void set_state(enum usb_disk_state state)
{
	struct gui_ctx *c = ctx_lock();
	bool changed = c->usb_disk.state != state;

	c->usb_disk.state = state;
	ctx_unlock(changed ? CTX_USB_DISK : 0);
}

static void take_back(enum usb_disk_state then)
{
	mainloop_timer_cancel(ud.ml, ud.poll);
	ud.poll = NULL;
	if (ud.shared) {
		LOGI(TAG, "taking the games partition back");
		if (run("stop") != CMD_OK)
			LOGW(TAG, "%s stop failed", ud.cmd);
		ud.shared = false;
	}
	set_state(then);
}

static void on_poll(void *unused)
{
	(void)unused;
	switch (run("status")) {
	case CMD_OK:
		set_state(USB_DISK_SHARED);
		break;
	case CMD_NO_HOST:
		set_state(USB_DISK_NO_HOST);
		break;
	case CMD_EJECTED:
		LOGI(TAG, "ejected by the computer");
		take_back(USB_DISK_EJECTED);
		break;
	default:
		LOGW(TAG, "no longer shared");
		take_back(USB_DISK_FAILED);
		break;
	}
}

static void share(void)
{
	int ret, i;

	if (!ud.cmd || ud.shared)
		return;
	set_state(USB_DISK_STARTING);
	for (i = 0; (ret = run("start")) == CMD_BUSY && i < BUSY_TRIES; i++)
		usleep(BUSY_WAIT_US);
	if (ret != CMD_OK) {
		LOGW(TAG, "%s start: %d", ud.cmd, ret);
		set_state(ret == CMD_BUSY ? USB_DISK_BUSY : USB_DISK_FAILED);
		return;
	}
	LOGI(TAG, "games partition shared");
	ud.shared = true;
	on_poll(NULL);
	if (ud.shared)
		ud.poll = mainloop_add_timer(ud.ml, POLL_MS, POLL_MS, on_poll, NULL);
}

static int usb_disk_init(const struct control_env *env)
{
	const char *cmd = getenv("BARE_APP_USB_DISK");
	struct gui_ctx *c;

	ud.ml = env->ml;
	ud.cmd = cmd && *cmd ? cmd : env->pf->usb_disk;
	if (ud.cmd && access(ud.cmd, X_OK) < 0)
		ud.cmd = NULL;
	c = ctx_lock();
	c->usb_disk.available = ud.cmd != NULL;
	c->usb_disk.state = USB_DISK_OFF;
	ctx_unlock(CTX_USB_DISK);
	return 0;
}

static void usb_disk_action(const struct gui_action *a)
{
	struct gui_ctx *c;
	bool shown;

	switch (a->type) {
	case ACT_OPEN_SCREEN:
		if (a->arg == SCREEN_USB_DISK)
			share();
		break;
	case ACT_USB_DISK:
		if (a->arg)
			share();
		else
			take_back(USB_DISK_OFF);
		break;
	case ACT_BACK:
		/* ctl_nav has run: is the screen still there? */
		c = ctx_lock();
		shown = ctx_screen(c) == SCREEN_USB_DISK;
		ctx_unlock(0);
		if (!shown)
			take_back(USB_DISK_OFF);
		break;
	default:
		break;
	}
}

static void usb_disk_deinit(void)
{
	take_back(USB_DISK_OFF);
}

const struct control_ops ctl_usb_disk = {
	.name = "usb-disk",
	.init = usb_disk_init,
	.on_action = usb_disk_action,
	.deinit = usb_disk_deinit,
};
