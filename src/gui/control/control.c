#include "control.h"

#include <poll.h>

#include "core/log.h"
#include "core/mainloop.h"
#include "core/util.h"

#define TAG "control"

static const struct control_ops *const modules[] = {
	&ctl_app, &ctl_nav, &ctl_system, &ctl_settings, &ctl_power, &ctl_display,
};

static struct mainloop *loop;

static void on_actions(int fd, short revents, void *ud)
{
	struct gui_action a;
	size_t i;

	(void)fd;
	(void)revents;
	(void)ud;
	action_ack();
	while (action_pop(&a)) {
		LOGD(TAG, "action %s %d", action_name(a.type), a.arg);
		for (i = 0; i < ARRAY_SIZE(modules); i++)
			if (modules[i]->on_action)
				modules[i]->on_action(&a);
	}
}

int control_start(const struct control_env *env)
{
	size_t i;

	loop = env->ml;
	for (i = 0; i < ARRAY_SIZE(modules); i++)
		if (modules[i]->init(env) < 0) {
			LOGE(TAG, "%s failed to start", modules[i]->name);
			return -1;
		}
	return mainloop_add_fd(loop, action_fd(), POLLIN, on_actions, NULL);
}

void control_stop(void)
{
	size_t i = ARRAY_SIZE(modules);

	mainloop_remove_fd(loop, action_fd());
	while (i--)
		if (modules[i]->deinit)
			modules[i]->deinit();
}
