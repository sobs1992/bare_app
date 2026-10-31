/*
 * Wi-Fi service (control thread). The daemon may start after us, restart
 * or never come (a board without the chip): until the driver opens it is
 * tried again every few seconds. The status is polled as well, since the
 * address comes from DHCP and the daemon sends no event for it.
 */
#include <poll.h>
#include <string.h>

#include "core/bus.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "hal/platform.h"
#include "services.h"

#define TAG "wifi"
#define POLL_MS 3000		/* also the retry period while the daemon is missing */

static struct {
	struct mainloop *ml;
	const struct hal_wifi_ops *ops;
	const void *params;
	void *h;
	int fd;
	bool enabled;
	struct ml_timer *timer;
	struct ev last;
	bool have_last;
} wf = { .fd = -1, .enabled = true };

static void post_status(void)
{
	struct ev e;

	memset(&e, 0, sizeof(e));
	e.type = EV_WIFI_STATUS;
	e.u.wifi.available = wf.h != NULL;
	e.u.wifi.enabled = wf.enabled;
	if (!wf.h || !wf.enabled || wf.ops->status(wf.h, &e.u.wifi.st) < 0)
		memset(&e.u.wifi.st, 0, sizeof(e.u.wifi.st));	/* HAL_WIFI_DOWN */
	if (wf.have_last && !memcmp(&wf.last, &e, sizeof(e)))
		return;
	wf.last = e;
	wf.have_last = true;
	LOGD(TAG, "%s, link %d \"%s\" %s", e.u.wifi.available ? "up" : "no daemon",
	     e.u.wifi.st.link, e.u.wifi.st.ssid, e.u.wifi.st.ip);
	bus_post(&e);
}

static void post(enum ev_type type)
{
	struct ev e = { .type = type };

	bus_post(&e);
}

static void drop(void)
{
	LOGI(TAG, "lost the Wi-Fi daemon");
	mainloop_remove_fd(wf.ml, wf.fd);
	wf.ops->close(wf.h);
	wf.h = NULL;
	wf.fd = -1;
	post_status();
}

static void on_fd(int fd, short revents, void *ud)
{
	uint32_t ev;

	(void)fd;
	(void)ud;
	ev = wf.ops->process(wf.h);
	if (ev & HAL_WIFI_EV_WRONG_KEY)
		post(EV_WIFI_WRONG_KEY);
	if (ev & HAL_WIFI_EV_SCAN)
		post(EV_WIFI_SCAN);
	if ((ev & HAL_WIFI_EV_LOST) || (revents & (POLLERR | POLLHUP)))
		drop();
	else if (ev & HAL_WIFI_EV_STATUS)
		post_status();
}

static void try_open(void)
{
	wf.h = wf.ops->open(wf.params);
	if (!wf.h)
		return;
	wf.fd = wf.ops->fd(wf.h);
	if (wf.fd >= 0)
		mainloop_add_fd(wf.ml, wf.fd, POLLIN, on_fd, NULL);
	wf.ops->set_enabled(wf.h, wf.enabled);	/* the setting wins over the boot state */
	post_status();
	post(EV_WIFI_SCAN);	/* the daemon may have results already */
}

static void on_timer(void *ud)
{
	(void)ud;
	if (!wf.h)
		try_open();
	else
		post_status();
}

int wifi_service_start(struct mainloop *ml, const struct platform *pf)
{
	wf.ml = ml;
	wf.ops = pf->wifi.ops;
	wf.params = pf->wifi.params;
	wf.have_last = false;
	if (!wf.ops)
		return 0;
	/* the first try runs in the loop, after control has set wifi_set_enabled() */
	wf.timer = mainloop_add_timer(ml, 0, POLL_MS, on_timer, NULL);
	return wf.timer ? 0 : -1;
}

void wifi_service_stop(void)
{
	if (!wf.ops)
		return;
	mainloop_timer_cancel(wf.ml, wf.timer);
	if (wf.h) {
		if (wf.fd >= 0)
			mainloop_remove_fd(wf.ml, wf.fd);
		wf.ops->close(wf.h);
	}
	wf.h = NULL;
	wf.fd = -1;
	wf.ops = NULL;
	wf.timer = NULL;
}

bool wifi_available(void)
{
	return wf.h != NULL;
}

void wifi_set_enabled(bool on)
{
	wf.enabled = on;
	if (!wf.h)
		return;
	wf.ops->set_enabled(wf.h, on);
	post_status();
	if (on)
		wifi_scan();
}

int wifi_scan(void)
{
	return wf.h && wf.enabled ? wf.ops->scan(wf.h) : -1;
}

int wifi_networks(struct hal_wifi_net *nets, int max)
{
	return wf.h && wf.enabled ? wf.ops->networks(wf.h, nets, max) : 0;
}

int wifi_connect(const char *ssid, const char *key)
{
	int ret = wf.h && wf.enabled ? wf.ops->connect(wf.h, ssid, key) : -1;

	post_status();
	return ret;
}

int wifi_forget(const char *ssid)
{
	int ret = wf.h ? wf.ops->forget(wf.h, ssid) : -1;

	post_status();
	return ret;
}
