/*
 * Two datagram sockets to the supplicant, like wpa_cli: one for requests
 * (blocking with a timeout; the supplicant answers in milliseconds), one
 * ATTACHed for its events.
 *
 * connect() uses SELECT_NETWORK, which disables every other network. They
 * are enabled again once the chosen one connects, fails with a wrong key
 * or does not connect in time, so saved networks keep connecting by
 * themselves. A new key always goes into a new entry, which replaces the
 * saved ones of that name only after it connects: a mistyped key never
 * costs a working one. Keys never go into the log.
 */
#include "wifi_wpa.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"

#define TAG "wpa"
#define REPLY_MAX 4096		/* the supplicant's own reply limit */
#define TIMEOUT_MS 2000
#define SELECT_TIMEOUT_MS 30000
#define MAX_IDS 32

struct wpa {
	const struct wifi_wpa_params *p;
	int cmd, ev;
	char cmd_path[64], ev_path[64];
	bool lost;
	int selecting;			/* network chosen by connect(), -1: none */
	bool added;			/* ...which connect() created */
	char selecting_ssid[HAL_SSID_MAX + 1];
	uint64_t selected_at;
	int reenable[MAX_IDS], n_reenable;	/* enabled before SELECT_NETWORK */
	char reply[REPLY_MAX + 1];
};

/* ---- text ------------------------------------------------------------- */

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

void wpa_unescape(const char *in, char *out, size_t size)
{
	size_t n = 0;
	int c;

	while (*in && n + 1 < size) {
		c = (unsigned char)*in++;
		if (c == '\\' && *in) {
			c = (unsigned char)*in++;
			switch (c) {
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'e': c = 27; break;
			case 'x':
				if (hexval(in[0]) >= 0 && hexval(in[1]) >= 0) {
					c = hexval(in[0]) << 4 | hexval(in[1]);
					in += 2;
				}
				break;
			default:	/* \\ \" and anything else: the character itself */
				break;
			}
		}
		out[n++] = (char)c;
	}
	out[n] = '\0';
}

/* Split a line at tabs, keeping empty fields; returns the field count */
static int split(char *line, char **f, int max)
{
	int n = 0;

	while (n < max) {
		f[n++] = line;
		line = strchr(line, '\t');
		if (!line)
			break;
		*line++ = '\0';
	}
	return n;
}

static bool starts(const char *s, const char *prefix)
{
	return !strncmp(s, prefix, strlen(prefix));
}

static int by_signal(const void *a, const void *b)
{
	return ((const struct hal_wifi_net *)b)->signal - ((const struct hal_wifi_net *)a)->signal;
}

int wpa_parse_scan(const char *results, const char *saved, struct hal_wifi_net *nets, int max)
{
	char *buf = strdup(results ? results : ""), *line, *save, *f[5];
	char ssid[HAL_SSID_MAX + 1];
	int n = 0, i, level, signal;

	if (!buf)
		return 0;
	/* bssid / frequency / signal level / flags / ssid; the header has no tabs */
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (split(line, f, 5) < 5)
			continue;
		wpa_unescape(f[4], ssid, sizeof(ssid));
		if (!ssid[0])
			continue;	/* hidden network */
		level = atoi(f[2]);
		signal = level < 0 ? CLAMP(2 * (level + 100), 0, 100) : CLAMP(level, 0, 100);
		for (i = 0; i < n && strcmp(nets[i].ssid, ssid); i++)
			;
		if (i == n) {
			if (n == max)
				continue;
			memset(&nets[n], 0, sizeof(nets[n]));
			memcpy(nets[n].ssid, ssid, sizeof(ssid));
			nets[n++].signal = -1;
		}
		nets[i].signal = MAX(nets[i].signal, signal);	/* the best access point */
		if (strstr(f[3], "WPA") || strstr(f[3], "RSN") || strstr(f[3], "WEP") ||
		    strstr(f[3], "SAE"))
			nets[i].secured = true;
	}
	free(buf);

	/* network id / ssid / bssid / flags */
	buf = strdup(saved ? saved : "");
	for (line = buf ? strtok_r(buf, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
		if (split(line, f, 4) < 4)
			continue;
		wpa_unescape(f[1], ssid, sizeof(ssid));
		for (i = 0; i < n; i++)
			if (!strcmp(nets[i].ssid, ssid))
				nets[i].saved = true;
	}
	free(buf);
	qsort(nets, n, sizeof(*nets), by_signal);
	return n;
}

/* ---- control socket --------------------------------------------------- */

static int ctrl_open(const char *server, char *local, size_t size)
{
	static int counter;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	snprintf(local, size, "/tmp/bare_app-wpa-%d-%d", (int)getpid(), counter++);
	unlink(local);
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", local);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		local[0] = '\0';
		return -1;
	}
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", server);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		unlink(local);
		local[0] = '\0';
		return -1;
	}
	return fd;
}

static void ctrl_close(int fd, char *local)
{
	if (fd >= 0)
		close(fd);
	if (local[0])
		unlink(local);
	local[0] = '\0';
}

/* Send a request, wait for its reply in buf (NUL-terminated); -1 on failure */
static int request_on(struct wpa *w, int fd, const char *req, char *buf, size_t size)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	uint64_t deadline = now_ms() + TIMEOUT_MS;
	char stale[64];
	ssize_t n;
	int left;

	while (fd == w->cmd && recv(fd, stale, sizeof(stale), 0) > 0)
		;	/* the late reply of a request that timed out */
	if (send(fd, req, strlen(req), 0) < 0) {
		LOGW(TAG, "%s: %s", w->p->ctrl, strerror(errno));
		w->lost = true;
		return -1;
	}
	for (;;) {
		left = (int)((int64_t)(deadline - now_ms()));
		if (left <= 0 || poll(&pfd, 1, left) == 0) {
			LOGW(TAG, "no answer from wpa_supplicant");
			return -1;
		}
		n = recv(fd, buf, size - 1, 0);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (n < 0) {
			w->lost = true;
			return -1;
		}
		buf[n] = '\0';
		if (fd == w->ev && buf[0] == '<')
			continue;	/* an event before the ATTACH answer */
		return (int)n;
	}
}

static int request(struct wpa *w, const char *req)
{
	return request_on(w, w->cmd, req, w->reply, sizeof(w->reply));
}

/* A command answered with "OK" */
__attribute__((format(printf, 2, 3)))
static int command(struct wpa *w, const char *fmt, ...)
{
	char req[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(req, sizeof(req), fmt, ap);
	va_end(ap);
	if (request(w, req) < 0)
		return -1;
	return starts(w->reply, "OK") ? 0 : -1;
}

/* ---- networks --------------------------------------------------------- */

/* Saved network ids with this name; returns their count */
static int saved_ids(struct wpa *w, const char *ssid, int *ids, int max, bool enabled_only)
{
	char *line, *save, *f[4], name[HAL_SSID_MAX + 1];
	int n = 0;

	if (request(w, "LIST_NETWORKS") < 0)
		return -1;
	for (line = strtok_r(w->reply, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (split(line, f, 4) < 4 || n == max)
			continue;
		wpa_unescape(f[1], name, sizeof(name));
		if (ssid && strcmp(name, ssid))
			continue;
		if (enabled_only && strstr(f[3], "[DISABLED]"))
			continue;
		ids[n++] = atoi(f[0]);
	}
	return n;
}

/*
 * Enable what SELECT_NETWORK disabled. failed: drop an entry connect()
 * added; otherwise it replaces older entries of the same name.
 */
static void end_select(struct wpa *w, bool failed)
{
	int old[MAX_IDS], n, i, j;

	if (w->selecting < 0)
		return;
	if (failed && w->added) {
		command(w, "REMOVE_NETWORK %d", w->selecting);
	} else if (w->added) {
		n = saved_ids(w, w->selecting_ssid, old, MAX_IDS, false);
		for (i = 0; i < n; i++) {
			if (old[i] == w->selecting)
				continue;
			command(w, "REMOVE_NETWORK %d", old[i]);
			for (j = 0; j < w->n_reenable; j++)
				if (w->reenable[j] == old[i])
					w->reenable[j] = -1;
		}
	}
	for (i = 0; i < w->n_reenable; i++)
		if (w->reenable[i] >= 0)
			command(w, "ENABLE_NETWORK %d", w->reenable[i]);
	w->selecting = -1;
	w->added = false;
	w->n_reenable = 0;
}

static void check_select_timeout(struct wpa *w)
{
	if (w->selecting >= 0 && now_ms() - w->selected_at > SELECT_TIMEOUT_MS) {
		LOGW(TAG, "network %d did not connect", w->selecting);
		end_select(w, true);
	}
}

/* ---- ops -------------------------------------------------------------- */

static void wpa_close(void *h)
{
	struct wpa *w = h;

	if (w->ev >= 0)
		send(w->ev, "DETACH", 6, 0);
	ctrl_close(w->cmd, w->cmd_path);
	ctrl_close(w->ev, w->ev_path);
	free(w);
}

static void *wpa_open(const void *params)
{
	const struct wifi_wpa_params *p = params;
	struct wpa *w = calloc(1, sizeof(*w));
	char buf[64];

	if (!w)
		return NULL;
	w->p = p;
	w->selecting = -1;
	w->cmd = ctrl_open(p->ctrl, w->cmd_path, sizeof(w->cmd_path));
	w->ev = w->cmd < 0 ? -1 : ctrl_open(p->ctrl, w->ev_path, sizeof(w->ev_path));
	if (w->ev < 0 || request_on(w, w->ev, "ATTACH", buf, sizeof(buf)) < 0 || !starts(buf, "OK")) {
		wpa_close(w);
		return NULL;	/* not running (yet): the caller tries again later */
	}
	LOGI(TAG, "attached to %s", p->ctrl);
	return w;
}

static int wpa_fd(void *h)
{
	return ((struct wpa *)h)->ev;
}

static uint32_t wpa_process(void *h)
{
	struct wpa *w = h;
	char buf[512], *msg;
	uint32_t ev = 0;
	ssize_t n;

	while ((n = recv(w->ev, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[n] = '\0';
		msg = buf[0] == '<' && strchr(buf, '>') ? strchr(buf, '>') + 1 : buf;	/* "<3>" level */
		if (starts(msg, "CTRL-EVENT-SCAN-RESULTS")) {
			ev |= HAL_WIFI_EV_SCAN;
		} else if (starts(msg, "CTRL-EVENT-SCAN-STARTED") || starts(msg, "CTRL-EVENT-BSS-")) {
			/* nothing to show */
		} else if (starts(msg, "CTRL-EVENT-TERMINATING")) {
			w->lost = true;
		} else if (starts(msg, "CTRL-EVENT-SSID-TEMP-DISABLED") && strstr(msg, "reason=WRONG_KEY")) {
			const char *id = strstr(msg, " id=");

			LOGI(TAG, "wrong key");
			ev |= HAL_WIFI_EV_WRONG_KEY | HAL_WIFI_EV_STATUS;
			if (id && atoi(id + 4) == w->selecting)
				end_select(w, true);
		} else {
			if (starts(msg, "CTRL-EVENT-CONNECTED") && w->selecting >= 0) {
				end_select(w, false);
				command(w, "SAVE_CONFIG");
			}
			ev |= HAL_WIFI_EV_STATUS;	/* state changes, association steps */
		}
	}
	check_select_timeout(w);
	return ev | (w->lost ? HAL_WIFI_EV_LOST : 0);
}

static int wpa_status(void *h, struct hal_wifi_status *st)
{
	struct wpa *w = h;
	char *line, *save, *v;

	memset(st, 0, sizeof(*st));
	st->link = HAL_WIFI_DISCONNECTED;
	check_select_timeout(w);
	if (request(w, "STATUS") < 0)
		return -1;
	for (line = strtok_r(w->reply, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		v = strchr(line, '=');
		if (!v)
			continue;
		*v++ = '\0';
		if (!strcmp(line, "wpa_state")) {
			if (!strcmp(v, "COMPLETED"))
				st->link = HAL_WIFI_CONNECTED;
			else if (!strcmp(v, "INTERFACE_DISABLED"))
				st->link = HAL_WIFI_DOWN;
			else if (!strcmp(v, "AUTHENTICATING") || !strcmp(v, "ASSOCIATING") ||
				 !strcmp(v, "ASSOCIATED") || strstr(v, "HANDSHAKE"))
				st->link = HAL_WIFI_CONNECTING;
		} else if (!strcmp(line, "ssid")) {
			wpa_unescape(v, st->ssid, sizeof(st->ssid));
		} else if (!strcmp(line, "ip_address")) {
			snprintf(st->ip, sizeof(st->ip), "%s", v);
		}
	}
	if (st->link != HAL_WIFI_CONNECTED)
		st->ip[0] = '\0';
	if (st->link < HAL_WIFI_CONNECTING)
		st->ssid[0] = '\0';
	return 0;
}

static int wpa_scan(void *h)
{
	struct wpa *w = h;

	if (request(w, "SCAN") < 0)
		return -1;
	return starts(w->reply, "OK") || starts(w->reply, "FAIL-BUSY") ? 0 : -1;	/* busy: one runs */
}

static int wpa_networks(void *h, struct hal_wifi_net *nets, int max)
{
	struct wpa *w = h;
	char *results;
	int n;

	if (request(w, "SCAN_RESULTS") < 0)
		return -1;
	results = strdup(w->reply);
	if (!results)
		return -1;
	if (request(w, "LIST_NETWORKS") < 0)
		w->reply[0] = '\0';
	n = wpa_parse_scan(results, w->reply, nets, max);
	free(results);
	return n;
}

static bool key_ok(const char *key)
{
	size_t n = strlen(key), i;

	for (i = 0; i < n; i++)
		if (key[i] < 0x20 || key[i] > 0x7e)
			return false;
	return n >= 8 && n <= HAL_WIFI_KEY_MAX;
}

static int wpa_connect(void *h, const char *ssid, const char *key)
{
	struct wpa *w = h;
	char hex[2 * HAL_SSID_MAX + 1];
	int id, i;

	if (!ssid[0] || (key && !key_ok(key)))
		return -1;
	end_select(w, true);		/* a previous choice still pending */
	if (!key && saved_ids(w, ssid, &id, 1, false) == 1) {
		w->added = false;
	} else {
		if (request(w, "ADD_NETWORK") < 0 || sscanf(w->reply, "%d", &id) != 1)
			return -1;
		w->added = true;
		for (i = 0; ssid[i] && i < HAL_SSID_MAX; i++)
			snprintf(hex + 2 * i, 3, "%02x", (unsigned char)ssid[i]);
		hex[2 * i] = '\0';
		if (command(w, "SET_NETWORK %d ssid %s", id, hex) < 0 ||
		    (key ? command(w, "SET_NETWORK %d psk \"%s\"", id, key)
			 : command(w, "SET_NETWORK %d key_mgmt NONE", id)) < 0) {
			command(w, "REMOVE_NETWORK %d", id);
			w->added = false;
			return -1;
		}
	}
	w->n_reenable = MAX(saved_ids(w, NULL, w->reenable, MAX_IDS, true), 0);
	if (command(w, "SELECT_NETWORK %d", id) < 0) {
		if (w->added)
			command(w, "REMOVE_NETWORK %d", id);
		w->added = false;
		w->n_reenable = 0;
		return -1;
	}
	w->selecting = id;
	snprintf(w->selecting_ssid, sizeof(w->selecting_ssid), "%s", ssid);
	w->selected_at = now_ms();
	LOGI(TAG, "connecting to network %d", id);
	return 0;
}

static int wpa_forget(void *h, const char *ssid)
{
	struct wpa *w = h;
	int ids[MAX_IDS], n, i;

	n = saved_ids(w, ssid, ids, MAX_IDS, false);
	for (i = 0; i < n; i++) {
		if (ids[i] == w->selecting)
			end_select(w, true);
		command(w, "REMOVE_NETWORK %d", ids[i]);
	}
	return n > 0 ? command(w, "SAVE_CONFIG") : -1;
}

static int wpa_set_enabled(void *h, bool on)
{
	struct wpa *w = h;

	if (!on)
		end_select(w, true);
	return command(w, on ? "RECONNECT" : "DISCONNECT");
}

const struct hal_wifi_ops wifi_wpa = {
	.name = "wifi-wpa",
	.open = wpa_open,
	.fd = wpa_fd,
	.process = wpa_process,
	.status = wpa_status,
	.scan = wpa_scan,
	.networks = wpa_networks,
	.connect = wpa_connect,
	.forget = wpa_forget,
	.set_enabled = wpa_set_enabled,
	.close = wpa_close,
};
