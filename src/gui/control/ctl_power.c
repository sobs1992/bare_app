/* Power: battery state into CTX_POWER, low battery and charger as notifications. */
#include <stddef.h>

#include "control.h"
#include "core/bus.h"
#include "gui/ctx.h"
#include "gui/notify.h"
#include "services/services.h"

#define LOW_BATTERY 15

static int sub;
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

	(void)env;
	c->power.available = power_available();
	ctx_unlock(CTX_POWER);
	sub = bus_subscribe(EV_POWER_CHANGED, on_power, NULL);
	return sub > 0 ? 0 : -1;
}

static void power_deinit(void)
{
	bus_unsubscribe(sub);
}

const struct control_ops ctl_power = {
	.name = "power",
	.init = power_init,
	.deinit = power_deinit,
};
