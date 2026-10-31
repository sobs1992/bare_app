/*
 * Wi-Fi: link and networks in range into CTX_WIFI, the Wi-Fi actions.
 * On/off is kept in the settings file; networks and keys are kept by the
 * Wi-Fi daemon. Keys from the view go straight to the service and are
 * never logged.
 */
#include <stdio.h>
#include <string.h>

#include "control.h"
#include "core/bus.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "gui/ctx.h"
#include "gui/notify.h"
#include "services/config.h"
#include "services/services.h"

#define TAG "wifi"
#define SCAN_TIMEOUT_MS 10000

static struct {
	struct mainloop *ml;
	int subs[3];
	char asked_for[WIFI_SSID_MAX + 1];	/* connect the user asked for, "" none */
	enum wifi_link link;
	struct ml_timer *scan_timeout;
} cw;

static enum wifi_link from_hal(enum hal_wifi_link l)
{
	switch (l) {
	case HAL_WIFI_CONNECTED:  return WIFI_CONNECTED;
	case HAL_WIFI_CONNECTING: return WIFI_CONNECTING;
	case HAL_WIFI_DISCONNECTED: return WIFI_DISCONNECTED;
	default:                  return WIFI_OFF;
	}
}

static void set_scanning(bool on)
{
	struct gui_ctx *c;

	if (cw.scan_timeout)
		mainloop_timer_cancel(cw.ml, cw.scan_timeout);
	cw.scan_timeout = NULL;
	c = ctx_lock();
	c->wifi.scanning = on;
	ctx_unlock(CTX_WIFI);
}

static void on_scan_timeout(void *ud)
{
	(void)ud;
	cw.scan_timeout = NULL;		/* one-shot: gone after this call */
	set_scanning(false);
}

/* Networks in range, the connected one first */
static void refresh_networks(void)
{
	struct hal_wifi_net nets[WIFI_MAX_NETS];
	struct gui_ctx *c;
	int n = wifi_networks(nets, WIFI_MAX_NETS), i, j = 0;

	c = ctx_lock();
	for (i = 0; i < n; i++)
		if (c->wifi.link >= WIFI_CONNECTING && !strcmp(nets[i].ssid, c->wifi.ssid))
			break;
	if (i < n) {
		memcpy(c->wifi.nets[j].ssid, nets[i].ssid, sizeof(c->wifi.nets[j].ssid));
		c->wifi.nets[j].signal = nets[i].signal;
		c->wifi.nets[j].secured = nets[i].secured;
		c->wifi.nets[j++].saved = nets[i].saved;
	}
	for (i = 0; i < n && j < WIFI_MAX_NETS; i++) {
		if (j > 0 && !strcmp(nets[i].ssid, c->wifi.nets[0].ssid))
			continue;
		memcpy(c->wifi.nets[j].ssid, nets[i].ssid, sizeof(c->wifi.nets[j].ssid));
		c->wifi.nets[j].signal = nets[i].signal;
		c->wifi.nets[j].secured = nets[i].secured;
		c->wifi.nets[j++].saved = nets[i].saved;
	}
	c->wifi.count = j;
	ctx_unlock(CTX_WIFI);
}

static void on_status(const struct ev *e, void *ud)
{
	enum wifi_link link = from_hal(e->u.wifi.st.link);
	struct gui_ctx *c;
	bool joined;

	(void)ud;
	c = ctx_lock();
	c->wifi.available = e->u.wifi.available;
	c->wifi.enabled = e->u.wifi.enabled;
	c->wifi.link = link;
	memcpy(c->wifi.ssid, e->u.wifi.st.ssid, sizeof(c->wifi.ssid));
	memcpy(c->wifi.ip, e->u.wifi.st.ip, sizeof(c->wifi.ip));
	if (link == WIFI_OFF) {
		c->wifi.count = 0;
		c->wifi.scanning = false;
	}
	ctx_unlock(CTX_WIFI);

	joined = link == WIFI_CONNECTED && cw.link != WIFI_CONNECTED;
	if (joined && cw.asked_for[0] && !strcmp(cw.asked_for, e->u.wifi.st.ssid))
		gui_notify_post(NOTIFY_WIFI_CONNECTED, 0);
	if (joined)
		cw.asked_for[0] = '\0';
	if (link != cw.link)
		refresh_networks();	/* the connected one moves up, a new one is saved */
	cw.link = link;
}

static void on_scan(const struct ev *e, void *ud)
{
	(void)e;
	(void)ud;
	set_scanning(false);
	refresh_networks();
}

static void on_wrong_key(const struct ev *e, void *ud)
{
	(void)e;
	(void)ud;
	cw.asked_for[0] = '\0';
	gui_notify_post(NOTIFY_WIFI_WRONG_KEY, 0);
	refresh_networks();	/* a network added with that key is gone again */
}

static int wifi_init(const struct control_env *env)
{
	struct gui_ctx *c;
	bool on = config_get_int("wifi", 1) != 0;

	memset(&cw, 0, sizeof(cw));
	cw.ml = env->ml;
	cw.subs[0] = bus_subscribe(EV_WIFI_STATUS, on_status, NULL);
	cw.subs[1] = bus_subscribe(EV_WIFI_SCAN, on_scan, NULL);
	cw.subs[2] = bus_subscribe(EV_WIFI_WRONG_KEY, on_wrong_key, NULL);
	if (cw.subs[0] <= 0 || cw.subs[1] <= 0 || cw.subs[2] <= 0)
		return -1;
	c = ctx_lock();
	c->wifi.enabled = on;
	ctx_unlock(CTX_WIFI);
	wifi_set_enabled(on);	/* the service applies it once the daemon answers */
	return 0;
}

static void wifi_action(const struct gui_action *a)
{
	struct gui_ctx *c;

	switch (a->type) {
	case ACT_WIFI_ENABLE:
		config_set_int("wifi", a->arg != 0);
		wifi_set_enabled(a->arg != 0);
		break;
	case ACT_WIFI_SCAN:
		if (wifi_scan() == 0) {
			set_scanning(true);
			cw.scan_timeout = mainloop_add_timer(cw.ml, SCAN_TIMEOUT_MS, 0, on_scan_timeout,
							     NULL);
		}
		break;
	case ACT_WIFI_ASK_KEY:
		c = ctx_lock();
		snprintf(c->wifi.asked, sizeof(c->wifi.asked), "%s", a->str);
		ctx_unlock(CTX_WIFI);
		break;
	case ACT_WIFI_CONNECT:
		LOGI(TAG, "connect to \"%s\"%s", a->str, a->secret[0] ? " with a new key" : "");
		if (wifi_connect(a->str, a->secret[0] ? a->secret : NULL) < 0) {
			gui_notify_post(NOTIFY_WIFI_FAILED, 0);
			break;
		}
		snprintf(cw.asked_for, sizeof(cw.asked_for), "%s", a->str);
		break;
	case ACT_WIFI_FORGET:
		LOGI(TAG, "forget \"%s\"", a->str);
		wifi_forget(a->str);
		refresh_networks();
		break;
	default:
		break;
	}
}

static void wifi_deinit(void)
{
	int i;

	if (cw.scan_timeout)
		mainloop_timer_cancel(cw.ml, cw.scan_timeout);
	for (i = 0; i < 3; i++)
		bus_unsubscribe(cw.subs[i]);
}

const struct control_ops ctl_wifi = {
	.name = "wifi",
	.init = wifi_init,
	.on_action = wifi_action,
	.deinit = wifi_deinit,
};
