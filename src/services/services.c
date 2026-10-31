#include "services.h"

#include <poll.h>
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
	const struct hal_mixer_ops *mixer_ops;
	void *mixer;
	int mixer_fd;
	struct hal_mixer_state mixer_last;
	bool have_mixer_last;
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

#ifdef CONFIG_POWER_UEVENT
static void on_power_uevent(const struct uevent *u, void *ud)
{
	(void)u;
	power_poll(ud);
}
#endif

/* Posts the mixer state if it changed */
static void mixer_poll(void)
{
	struct ev e;

	memset(&e, 0, sizeof(e));
	e.type = EV_MIXER_CHANGED;
	if (sv.mixer_ops->read(sv.mixer, &e.u.mixer) < 0)
		return;
	if (sv.have_mixer_last && !memcmp(&sv.mixer_last, &e.u.mixer, sizeof(sv.mixer_last)))
		return;
	sv.mixer_last = e.u.mixer;
	sv.have_mixer_last = true;
	LOGD(TAG, "mixer: %d%%, %s%s", e.u.mixer.volume,
	     e.u.mixer.route == HAL_ROUTE_HEADPHONES ? "headphones" : "speaker",
	     e.u.mixer.jack ? ", jack in" : "");
	bus_post(&e);
}

static void on_mixer(int fd, short revents, void *ud)
{
	(void)fd;
	(void)revents;
	(void)ud;
	mixer_poll();
}

int services_start(struct mainloop *ml, const struct platform *pf)
{
	memset(&sv, 0, sizeof(sv));
	sv.ml = ml;
	if (pf->power.ops) {
		sv.power_ops = pf->power.ops;
		sv.power = sv.power_ops->open(pf->power.params);
		if (sv.power) {
#ifdef CONFIG_POWER_UEVENT
			sv.power_mon = uevent_open(ml, "power_supply", on_power_uevent, NULL);
#endif
			sv.power_timer = mainloop_add_timer(ml, 0, sv.power_mon ? POWER_POLL_MS
										: POWER_POLL_NO_UEVENT_MS,
							    power_poll, NULL);
		}
	}
	if (pf->backlight.ops) {
		sv.bl_ops = pf->backlight.ops;
		sv.bl = sv.bl_ops->open(pf->backlight.params);
	}
	sv.mixer_fd = -1;
	if (pf->mixer.ops) {
		sv.mixer_ops = pf->mixer.ops;
		sv.mixer = sv.mixer_ops->open(pf->mixer.params);
		sv.mixer_fd = sv.mixer ? sv.mixer_ops->fd(sv.mixer) : -1;
		if (sv.mixer_fd >= 0)
			mainloop_add_fd(ml, sv.mixer_fd, POLLIN, on_mixer, NULL);
		if (sv.mixer)
			mixer_poll();
	}
	LOGI(TAG, "power %s, backlight %s, mixer %s", sv.power ? "yes" : "no", sv.bl ? "yes" : "no",
	     sv.mixer ? "yes" : "no");
	sound_start(pf);
	return wifi_service_start(ml, pf);
}

void services_stop(void)
{
	if (sv.power) {
#ifdef CONFIG_POWER_UEVENT
		uevent_close(sv.power_mon);
#endif
		mainloop_timer_cancel(sv.ml, sv.power_timer);
		sv.power_ops->close(sv.power);
	}
	if (sv.bl)
		sv.bl_ops->close(sv.bl);
	if (sv.mixer_fd >= 0)
		mainloop_remove_fd(sv.ml, sv.mixer_fd);
	if (sv.mixer)
		sv.mixer_ops->close(sv.mixer);
	wifi_service_stop();
	sound_stop();
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

bool audio_available(void)
{
	return sv.mixer != NULL;
}

/* What was posted last: reading the driver here would eat its change events */
int audio_get(struct hal_mixer_state *st)
{
	if (!sv.have_mixer_last)
		return -1;
	*st = sv.mixer_last;
	return 0;
}

int audio_set_volume(int percent)
{
	int ret = sv.mixer ? sv.mixer_ops->set_volume(sv.mixer, percent) : -1;

	if (sv.mixer)
		mixer_poll();	/* drivers without change events */
	return ret;
}

int audio_set_route(enum hal_audio_route route)
{
	int ret = sv.mixer ? sv.mixer_ops->set_route(sv.mixer, route) : -1;

	if (sv.mixer)
		mixer_poll();
	return ret;
}
