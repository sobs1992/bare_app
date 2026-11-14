/*
 * USB motion sensor (input_imu.h). The port is found in sysfs by the USB
 * ids of its device (/sys/class/tty/ttyACMn/device is the interface, its
 * parent the device), at start and on every "add" uevent of a tty; mdev
 * makes the node a little after the uevent, so the scan is retried a few
 * times. The port is raw, and poll() wakes only for a few frames at a time
 * (VMIN): about 280 wakeups a second instead of 1100.
 *
 * Opening the port sets DTR, which starts the stream; 's' asks for it too
 * and 'i' for the status packet (the sample's units: full scales, rate).
 */
#include "input_imu.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "core/input.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "core/uevent.h"

#define TAG "imu"
#define SYNC0 0xA5
#define SYNC1 0x5A
#define PKT_IMU 0x01
#define PKT_STATUS 0x02
#define IMU_LEN 28
#define STATUS_LEN 36
#define FRAME_OVERHEAD 6
#define STATUS_IMU_OK 1u
#define STATUS_STREAM 8u
#define NOMINAL_US (1e6f / 1125)
#define SPAN_SAMPLES 2000	/* the period is measured over that many */
#define WAKE_FRAMES 4		/* VMIN: wake for this many samples */
#define RETRY_MS 100
#define RETRIES 20
#define SILENT_MS 2000		/* no samples that long: ask for the stream again */

/* ---- the stream --------------------------------------------------------- */

uint16_t imu_crc16(const uint8_t *p, size_t n)
{
	uint16_t crc = 0xFFFF;

	while (n--) {
		crc ^= (uint16_t)(*p++ << 8);
		for (int i = 0; i < 8; i++)
			crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}

static uint16_t u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static int16_t s16(const uint8_t *p)
{
	return (int16_t)u16(p);
}

static uint32_t u32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

void imu_parser_init(struct imu_parser *p)
{
	memset(p, 0, sizeof(*p));
	/* until the status says otherwise: the firmware's defaults */
	p->gyro_scale = 2000.0f / 32768;
	p->accel_scale = 4.0f / 32768;
	p->period_us = NOMINAL_US;
}

static void on_status(struct imu_parser *p, const uint8_t *s)
{
	uint8_t flags = s[3];
	uint32_t odr_mhz = u32(s + 4);
	uint16_t gyro_fs = u16(s + 8);
	uint8_t accel_fs = s[10];

	if (gyro_fs)
		p->gyro_scale = gyro_fs / 32768.0f;
	if (accel_fs)
		p->accel_scale = accel_fs / 32768.0f;
	if (!p->have_status && odr_mhz)
		p->period_us = 1e9f / odr_mhz;	/* nominal, until measured */
	if (!p->have_status || p->imu_ok != !!(flags & STATUS_IMU_OK))
		LOGI(TAG, "protocol %u, firmware %u.%u, IMU %s, gyro %u deg/s, accel %u g",
		     s[0], s[1], s[2], flags & STATUS_IMU_OK ? "ok" : "missing", gyro_fs, accel_fs);
	p->have_status = true;
	p->imu_ok = flags & STATUS_IMU_OK;
	p->streaming = flags & STATUS_STREAM;
}

static void on_imu(struct imu_parser *p, const uint8_t *s)
{
	uint32_t t = u32(s);
	uint16_t seq = u16(s + 4);
	float accel[3], gyro[3], dt;
	int k;

	for (k = 0; k < 3; k++) {
		accel[k] = s16(s + 6 + 2 * k) * p->accel_scale;
		gyro[k] = s16(s + 12 + 2 * k) * p->gyro_scale;
	}
	dt = p->period_us * 1e-6f;
	if (p->have_prev) {
		uint16_t gap = (uint16_t)(seq - p->prev_seq);
		uint32_t span = t - p->prev_t;

		if (gap >= 1 && gap < 1000) {	/* else lost track: one period */
			dt = gap * p->period_us * 1e-6f;
			p->span_us += span;
			p->span_n += gap;
			if (p->span_n >= SPAN_SAMPLES) {
				float measured = (float)p->span_us / p->span_n;

				if (measured > 0.5f * NOMINAL_US && measured < 2 * NOMINAL_US) {
					p->period_us = p->period_measured
							       ? 0.7f * p->period_us + 0.3f * measured
							       : measured;
					p->period_measured = true;
				}
				p->span_us = 0;
				p->span_n = 0;
			}
		} else if (gap == 0) {
			return;		/* the same sample again */
		}
	}
	p->have_prev = true;
	p->prev_t = t;
	p->prev_seq = seq;
	p->samples++;
	input_report_motion(gyro, accel, dt);
}

void imu_parser_feed(struct imu_parser *p, const uint8_t *data, size_t n)
{
	size_t i;

	while (n) {
		size_t room = sizeof(p->buf) - p->len, take = n < room ? n : room;

		memcpy(p->buf + p->len, data, take);
		p->len += take;
		data += take;
		n -= take;
		i = 0;
		while (p->len - i >= FRAME_OVERHEAD) {
			uint8_t *f = p->buf + i, type = f[2], len = f[3];

			if (f[0] != SYNC0 || f[1] != SYNC1) {
				i++;
				continue;
			}
			if (p->len - i < (size_t)len + FRAME_OVERHEAD)
				break;	/* the rest comes later */
			if (imu_crc16(f + 2, 2u + len) != u16(f + 4 + len)) {
				p->bad_crc++;
				i++;	/* not a frame: look for the next sync */
				continue;
			}
			if (type == PKT_IMU && len == IMU_LEN)
				on_imu(p, f + 4);
			else if (type == PKT_STATUS && len == STATUS_LEN)
				on_status(p, f + 4);
			i += (size_t)len + FRAME_OVERHEAD;
		}
		memmove(p->buf, p->buf + i, p->len - i);
		p->len -= i;
	}
	input_report_motion_end();
}

/* ---- the port ----------------------------------------------------------- */

struct imu {
	struct mainloop *ml;
	struct imu_params params;
	int fd;
	char node[32];
	struct imu_parser parser;
	uint32_t seen;		/* parser.samples at the last check */
	bool told_zero, told_rate;	/* logged: the gyro's zero, the rate measured */
	struct uevent_mon *mon;
	struct ml_timer *retry, *watch;
	int retries;
};

static void report_present(struct imu *h)
{
	bool ok = h->fd >= 0 && h->parser.samples > 0;

	input_motion_present(ok, ok ? 1e6f / h->parser.period_us : 0);
}

static void detach(struct imu *h)
{
	if (h->fd < 0)
		return;
	LOGI(TAG, "%s went away (%u samples, %u bad frames)", h->node, h->parser.samples,
	     h->parser.bad_crc);
	mainloop_remove_fd(h->ml, h->fd);
	close(h->fd);
	h->fd = -1;
	report_present(h);
}

static void send_cmd(struct imu *h, const char *cmd)
{
	if (h->fd >= 0 && write(h->fd, cmd, strlen(cmd)) < 0)
		LOGD(TAG, "%s: %s", h->node, strerror(errno));
}

static void on_readable(int fd, short revents, void *ud)
{
	struct imu *h = ud;
	uint8_t buf[1024];
	ssize_t n;
	bool had = h->parser.samples > 0;

	while ((n = read(fd, buf, sizeof(buf))) > 0)
		imu_parser_feed(&h->parser, buf, (size_t)n);
	if (!had && h->parser.samples)
		report_present(h);
	if ((n < 0 && errno != EAGAIN) || n == 0 || (revents & (POLLERR | POLLHUP)))
		detach(h);	/* unplugged */
}

/* Every second: the rate measured, and the stream asked for if it stopped */
static void on_watch(void *ud)
{
	struct imu *h = ud;

	if (h->fd < 0)
		return;
	if (h->parser.samples == h->seen) {
		if (h->parser.samples)
			LOGW(TAG, "%s: no samples", h->node);
		send_cmd(h, "s");
	}
	h->seen = h->parser.samples;
	report_present(h);
	if (!h->told_rate && h->parser.period_measured) {
		h->told_rate = true;
		LOGI(TAG, "%s: %.1f samples a second", h->node, 1e6 / h->parser.period_us);
	}
	{
		struct input_motion_info mi;

		input_motion_info(&mi);
		if (mi.bias_known != h->told_zero)
			LOGI(TAG, "gyro zero %s", mi.bias_known ? "known" : "not known yet");
		h->told_zero = mi.bias_known;
	}
}

static bool read_attr(const char *dir, const char *name, char *buf, size_t size)
{
	char path[384];
	FILE *f;
	bool ok;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (!(f = fopen(path, "r")))
		return false;
	ok = fgets(buf, (int)size, f) != NULL;
	fclose(f);
	if (ok)
		buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

/* /sys/class/tty/<tty>/device/.. is the USB device with its ids */
static bool is_ours(const struct imu *h, const char *tty)
{
	char dir[320], v[64];

	snprintf(dir, sizeof(dir), "/sys/class/tty/%.256s/device/..", tty);
	if (!read_attr(dir, "idVendor", v, sizeof(v)) || strtoul(v, NULL, 16) != h->params.vid)
		return false;
	if (!read_attr(dir, "idProduct", v, sizeof(v)) || strtoul(v, NULL, 16) != h->params.pid)
		return false;
	return !h->params.product ||
	       (read_attr(dir, "product", v, sizeof(v)) && !strcmp(v, h->params.product));
}

static bool open_port(struct imu *h, const char *tty)
{
	struct termios tio;
	char path[sizeof(((struct imu *)0)->node)];
	int fd;

	if (snprintf(path, sizeof(path), "/dev/%s", tty) >= (int)sizeof(path))
		return false;
	fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		if (errno != ENOENT)
			LOGW(TAG, "%s: %s", path, strerror(errno));
		return false;
	}
	if (tcgetattr(fd, &tio) == 0) {
		cfmakeraw(&tio);
		tio.c_cflag |= CLOCAL | CREAD;
		tio.c_cc[VMIN] = WAKE_FRAMES * (IMU_LEN + FRAME_OVERHEAD);
		tio.c_cc[VTIME] = 0;
		tcsetattr(fd, TCSANOW, &tio);
	}
	tcflush(fd, TCIFLUSH);
	h->fd = fd;
	memcpy(h->node, path, sizeof(h->node));
	imu_parser_init(&h->parser);
	h->seen = 0;
	h->told_rate = false;
	mainloop_add_fd(h->ml, fd, POLLIN, on_readable, h);
	send_cmd(h, "si");
	LOGI(TAG, "%s: motion sensor", path);
	return true;
}

static bool scan(struct imu *h)
{
	struct dirent *e;
	DIR *d;
	bool found = false;

	if (h->fd >= 0 || !(d = opendir("/sys/class/tty")))
		return h->fd >= 0;
	while (!found && (e = readdir(d)) != NULL)
		if (!strncmp(e->d_name, "ttyACM", 6) && is_ours(h, e->d_name))
			found = open_port(h, e->d_name);
	closedir(d);
	return found;
}

#ifdef CONFIG_INPUT_HOTPLUG
static void on_retry(void *ud)
{
	struct imu *h = ud;

	if (scan(h) || --h->retries <= 0) {
		mainloop_timer_cancel(h->ml, h->retry);
		h->retry = NULL;
	}
}

static void on_uevent(const struct uevent *u, void *ud)
{
	struct imu *h = ud;

	if (strcmp(u->action, "add") || !u->devname || strncmp(u->devname, "ttyACM", 6) ||
	    h->fd >= 0)
		return;
	h->retries = RETRIES;
	if (!h->retry)
		h->retry = mainloop_add_timer(h->ml, RETRY_MS, RETRY_MS, on_retry, h);
}
#endif

static void *imu_open(const void *params, struct mainloop *ml)
{
	struct imu *h = calloc(1, sizeof(*h));

	if (!h)
		return NULL;
	h->ml = ml;
	h->params = *(const struct imu_params *)params;
	h->fd = -1;
#ifdef CONFIG_INPUT_HOTPLUG
	h->mon = uevent_open(ml, "tty", on_uevent, h);
#endif
	h->watch = mainloop_add_timer(ml, SILENT_MS, 1000, on_watch, h);
	scan(h);
	return h;
}

static void imu_close(void *p)
{
	struct imu *h = p;

#ifdef CONFIG_INPUT_HOTPLUG
	mainloop_timer_cancel(h->ml, h->retry);
	uevent_close(h->mon);
#endif
	mainloop_timer_cancel(h->ml, h->watch);
	if (h->fd >= 0) {
		mainloop_remove_fd(h->ml, h->fd);
		close(h->fd);
	}
	input_motion_present(false, 0);
	free(h);
}

const struct hal_input_ops input_imu = {
	.name = "imu",
	.open = imu_open,
	.close = imu_close,
};
