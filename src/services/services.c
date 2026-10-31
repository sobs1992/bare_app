#include "services.h"

#include <string.h>

#include "core/bus.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "core/uevent.h"
#include "hal/platform.h"

#define TAG "services"
/* Charger and status changes come as uevents; drivers do not always send
 * one for every capacity step, so poll slowly as well */
#define POWER_POLL_MS 30000
#define POWER_POLL_NO_UEVENT_MS 5000

static struct {
	struct mainloop *ml;
	const struct hal_power_ops *power_ops;
	void *power;
	struct hal_power_state last;
	bool have_last;
	struct ml_timer *power_timer;
	struct uevent_mon *power_mon;
	const struct hal_backlight_ops *bl_ops;
	void *bl;
} sv;

static void power_poll(void *ud)
{
	struct ev e = { .type = EV_POWER_CHANGED };

	(void)ud;
	if (sv.power_ops->read(sv.power, &e.u.power) < 0)
		return;
	if (sv.have_last && !memcmp(&sv.last, &e.u.power, sizeof(sv.last)))
		return;
	sv.last = e.u.power;
	sv.have_last = true;
	LOGD(TAG, "power: %d%%%s%s", e.u.power.capacity, e.u.power.charging ? " charging" : "",
	     e.u.power.online ? " online" : "");
	bus_post(&e);
}

static void on_power_uevent(const struct uevent *u, void *ud)
{
	(void)u;
	power_poll(ud);
}

int services_start(struct mainloop *ml, const struct platform *pf)
{
	memset(&sv, 0, sizeof(sv));
	sv.ml = ml;
	if (pf->power.ops) {
		sv.power_ops = pf->power.ops;
		sv.power = sv.power_ops->open(pf->power.params);
		if (sv.power) {
			sv.power_mon = uevent_open(ml, "power_supply", on_power_uevent, NULL);
			sv.power_timer = mainloop_add_timer(ml, 0, sv.power_mon ? POWER_POLL_MS
										: POWER_POLL_NO_UEVENT_MS,
							    power_poll, NULL);
		}
	}
	if (pf->backlight.ops) {
		sv.bl_ops = pf->backlight.ops;
		sv.bl = sv.bl_ops->open(pf->backlight.params);
	}
	LOGI(TAG, "power %s, backlight %s", sv.power ? "yes" : "no", sv.bl ? "yes" : "no");
	return 0;
}

void services_stop(void)
{
	if (sv.power) {
		uevent_close(sv.power_mon);
		mainloop_timer_cancel(sv.ml, sv.power_timer);
		sv.power_ops->close(sv.power);
	}
	if (sv.bl)
		sv.bl_ops->close(sv.bl);
	memset(&sv, 0, sizeof(sv));
}

bool power_available(void)
{
	return sv.power != NULL;
}

bool backlight_available(void)
{
	return sv.bl != NULL;
}

int backlight_get(void)
{
	return sv.bl ? sv.bl_ops->get(sv.bl) : -1;
}

int backlight_set(int percent)
{
	return sv.bl ? sv.bl_ops->set(sv.bl, percent) : -1;
}
