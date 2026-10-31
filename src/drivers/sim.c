#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "core/util.h"

/* ---- mixer: values in memory, no jack --------------------------------- */

static struct hal_mixer_state mixer = { .volume = 60, .has_route = true };

static void *msim_open(const void *params)
{
	(void)params;
	return &mixer;
}

static int msim_fd(void *h)
{
	(void)h;
	return -1;
}

static int msim_read(void *h, struct hal_mixer_state *st)
{
	*st = *(struct hal_mixer_state *)h;
	return 0;
}

static int msim_set_volume(void *h, int percent)
{
	((struct hal_mixer_state *)h)->volume = CLAMP(percent, 0, 100);
	return 0;
}

static int msim_set_route(void *h, enum hal_audio_route route)
{
	((struct hal_mixer_state *)h)->route = route;
	return 0;
}

static void msim_close(void *h)
{
	(void)h;
}

const struct hal_mixer_ops mixer_sim = {
	.name = "mixer-sim",
	.open = msim_open,
	.fd = msim_fd,
	.read = msim_read,
	.set_volume = msim_set_volume,
	.set_route = msim_set_route,
	.close = msim_close,
};

/* ---- Wi-Fi: made-up networks, answers after a short delay -------------- */

#define SIM_NETS 4
#define SCAN_MS 600
#define CONNECT_MS 1200

static const struct hal_wifi_net sim_nets[SIM_NETS] = {
	{ "Home", 82, true, true },
	{ "Cafe guests", 64, false, false },
	{ "Neighbour", 41, true, false },
	{ "Printer-3F2A", 23, true, false },
};

struct wsim {
	int tfd;
	bool on;
	struct hal_wifi_status st;
	bool saved[SIM_NETS];
	enum { IDLE, SCANNING, JOINING } pending;
	bool key_right;
	uint32_t events;
};

static void arm(struct wsim *s, int ms)
{
	struct itimerspec t = { .it_value = { ms / 1000, (ms % 1000) * 1000000L } };

	timerfd_settime(s->tfd, 0, &t, NULL);
}

static int find(const char *ssid)
{
	int i;

	for (i = 0; i < SIM_NETS; i++)
		if (!strcmp(sim_nets[i].ssid, ssid))
			return i;
	return -1;
}

static void *wsim_open(const void *params)
{
	struct wsim *s = calloc(1, sizeof(*s));

	(void)params;
	if (!s)
		return NULL;
	s->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (s->tfd < 0) {
		free(s);
		return NULL;
	}
	s->on = true;
	s->saved[0] = true;
	s->st = (struct hal_wifi_status){ HAL_WIFI_CONNECTED, "Home", "192.168.1.23" };
	return s;
}

static int wsim_fd(void *h)
{
	return ((struct wsim *)h)->tfd;
}

static uint32_t wsim_process(void *h)
{
	struct wsim *s = h;
	uint64_t n;
	uint32_t ev;

	if (read(s->tfd, &n, sizeof(n)) != sizeof(n))
		return 0;
	if (s->pending == SCANNING) {
		s->events |= HAL_WIFI_EV_SCAN;
	} else if (s->pending == JOINING && s->key_right) {
		s->st.link = HAL_WIFI_CONNECTED;
		snprintf(s->st.ip, sizeof(s->st.ip), "192.168.1.%d", 20 + find(s->st.ssid));
		s->saved[find(s->st.ssid)] = true;
		s->events |= HAL_WIFI_EV_STATUS;
	} else if (s->pending == JOINING) {
		s->st = (struct hal_wifi_status){ HAL_WIFI_DISCONNECTED, "", "" };
		s->events |= HAL_WIFI_EV_STATUS | HAL_WIFI_EV_WRONG_KEY;
	}
	s->pending = IDLE;
	ev = s->events;
	s->events = 0;
	return ev;
}

static int wsim_status(void *h, struct hal_wifi_status *st)
{
	struct wsim *s = h;

	*st = s->st;
	if (!s->on)
		st->link = HAL_WIFI_DOWN;
	return 0;
}

static int wsim_scan(void *h)
{
	struct wsim *s = h;

	if (s->pending != IDLE)
		return -1;
	s->pending = SCANNING;
	arm(s, SCAN_MS);
	return 0;
}

static int wsim_networks(void *h, struct hal_wifi_net *nets, int max)
{
	struct wsim *s = h;
	int i, n = MIN(max, SIM_NETS);

	if (!s->on)
		return 0;
	for (i = 0; i < n; i++) {
		nets[i] = sim_nets[i];
		nets[i].saved = s->saved[i];
	}
	return n;
}

static int wsim_connect(void *h, const char *ssid, const char *key)
{
	struct wsim *s = h;
	int i = find(ssid);

	if (i < 0 || !s->on || (key && strlen(key) < 8))
		return -1;
	s->key_right = !sim_nets[i].secured || (key ? !strncmp(key, "pass", 4) : s->saved[i]);
	s->st = (struct hal_wifi_status){ HAL_WIFI_CONNECTING, "", "" };
	snprintf(s->st.ssid, sizeof(s->st.ssid), "%s", ssid);
	s->pending = JOINING;
	arm(s, CONNECT_MS);
	return 0;
}

static int wsim_forget(void *h, const char *ssid)
{
	struct wsim *s = h;
	int i = find(ssid);

	if (i < 0)
		return -1;
	s->saved[i] = false;
	if (!strcmp(s->st.ssid, ssid))
		s->st = (struct hal_wifi_status){ HAL_WIFI_DISCONNECTED, "", "" };
	return 0;
}

static int wsim_set_enabled(void *h, bool on)
{
	struct wsim *s = h;
	int i;

	if (on == s->on)
		return 0;
	s->on = on;
	s->st = (struct hal_wifi_status){ HAL_WIFI_DISCONNECTED, "", "" };
	s->pending = IDLE;
	for (i = 0; on && i < SIM_NETS; i++)
		if (s->saved[i])	/* like a supplicant: back to a saved network */
			return wsim_connect(s, sim_nets[i].ssid, NULL);
	return 0;
}

static void wsim_close(void *h)
{
	struct wsim *s = h;

	close(s->tfd);
	free(s);
}

const struct hal_wifi_ops wifi_sim = {
	.name = "wifi-sim",
	.open = wsim_open,
	.fd = wsim_fd,
	.process = wsim_process,
	.status = wsim_status,
	.scan = wsim_scan,
	.networks = wsim_networks,
	.connect = wsim_connect,
	.forget = wsim_forget,
	.set_enabled = wsim_set_enabled,
	.close = wsim_close,
};
