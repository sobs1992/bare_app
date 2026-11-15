/*
 * USB motion sensor (input_imu.h). The port is found in sysfs by the USB
 * ids of its device (/sys/class/tty/ttyACMn/device is the interface, its
 * parent the device), at start and on every "add" uevent of a tty; mdev
 * makes the node a little after the uevent, so the scan is retried a few
 * times. The port is raw, and while samples flow poll() wakes only for a
 * batch of them at a time (VMIN as long as the device's PKT_SAMPLES frames;
 * 1 while none come).
 *
 * Opening the port sets DTR, which starts the stream; 's' asks for it too
 * and 'i' for the info and the status (the sensor, its units and rate). The
 * sensor is known to core/input as "<name>-<USB serial>", so each one keeps
 * its own calibration.
 */
#include "input_imu.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "core/input.h"
#include "core/log.h"
#include "core/mainloop.h"
#include "core/motion.h"
#include "core/uevent.h"
#include "core/util.h"

#define TAG "imu"
#define SYNC0 0xA5
#define SYNC1 0x5A
#define PROTO 3			/* the only one read (firmware 0.2 on) */
#define PKT_STATUS 0x02
#define PKT_INFO 0x03
#define PKT_SAMPLES 0x04
#define PKT_MAG 0x05
#define STATUS_LEN 36
#define INFO_LEN 60
#define SAMPLES_HDR 8
#define SAMPLE_LEN 12
#define FRAME_OVERHEAD 6
#define STATUS_IMU_OK 1u
#define STATUS_STREAM 8u
#define STATUS_FAKE 16u
#define DEFAULT_US 1000.0f	/* the period until PKT_INFO says it */
#define SPAN_US 250000u		/* the period is measured anew over that much time */
#define JITTER_US 500.0f	/* an interval off by more: a pause (the times' delay: ~0.35 ms seen) */
#define WINDOW_MOST 3.0f	/* an interval adds to the window at most this many of its samples' periods */
#define RATE_SAME 0.02f		/* a window this close to the period is of its rate */
#define GAP_MISSING 3		/* samples missing (by number or by time) that are no gap yet */
#define RATE_SLOWEST 2.02f	/* a measured period is taken within 1/x..x of the info's (2x, rounding) */
#define RETRY_MS 100
#define RETRIES 20
#define SILENT_MS 2000		/* the first check of the stream */
#define LIVE_MS 1500		/* no samples that long (or 3 periods): not live, ask again */

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

static float f32(const uint8_t *p)
{
	uint32_t v = u32(p);
	float f;

	memcpy(&f, &v, sizeof(f));
	return isfinite(f) ? f : 0;
}

void imu_parser_init(struct imu_parser *p)
{
	memset(p, 0, sizeof(*p));
	p->nominal_us = p->period_us = DEFAULT_US;	/* no units, no samples until PKT_INFO */
}

/* A sensor (or its rate, units, channels) as the info says it anew: a new
 * stream, its rate measured anew (its first sample a gap) */
static void new_stream(struct imu_parser *p, uint32_t odr_mhz)
{
	float us = odr_mhz ? 1e9f / odr_mhz : DEFAULT_US;

	p->nominal_us = p->period_us = us;
	p->period_measured = false;
	p->period_refused = 0;	/* (of the rate before) */
	p->have_prev = p->have_real = false;
	p->span_us = 0;
	p->span_n = 0;
	p->win_n = p->win_head = 0;
}

static void on_status(struct imu_parser *p, const uint8_t *s)
{
	uint8_t flags = s[3];
	bool ok = flags & STATUS_IMU_OK;

	if (s[0] != PROTO) {
		/* firmware 0.1 (protocol 2): nothing of it is read */
		if (p->proto != s[0])
			LOGW(TAG, "protocol %u, firmware %u.%u: not supported, update the sensor's "
			     "firmware (protocol %u)", s[0], s[1], s[2], PROTO);
		p->proto = s[0];
		p->have_info = false;
		return;
	}
	if (!p->have_status || p->imu_ok != ok || p->proto != s[0])
		LOGI(TAG, "protocol %u, firmware %u.%u, sensor %s%s", s[0], s[1], s[2],
		     ok ? "ok" : "missing", flags & STATUS_FAKE ? " (fake)" : "");
	p->proto = s[0];
	p->have_status = true;
	p->imu_ok = ok;
	p->fake = flags & STATUS_FAKE;
	p->streaming = flags & STATUS_STREAM;
}

/* The sensor changed: core/input learns it before its samples */
static void identify(struct imu_parser *p)
{
	char id[INPUT_MOTION_ID_LEN];
	struct input_motion_desc d = {
		.id = id, .name = p->name,
		.channels = (p->channels & 1 ? MOTION_CH_GYRO : 0) | (p->channels & 2 ? MOTION_CH_ACCEL : 0),
		.gyro_noise = p->gyro_noise, .accel_noise = p->accel_noise,
		.rate_hz = p->nominal_us > 0 ? 1e6f / p->nominal_us : 0,
	};

	snprintf(id, sizeof(id), "%s-%s", p->name, p->serial[0] ? p->serial : "noserial");
	for (char *c = id; *c; c++)
		if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
		      *c == '-'))
			*c = '_';
	input_motion_sensor(&d);
}

/* What the sensor is */
static void on_info(struct imu_parser *p, const uint8_t *s)
{
	char name[sizeof(p->name)] = "";
	uint8_t id = s[3], ch = s[20];
	float gs = f32(s + 28), as = f32(s + 32), noise = f32(s + 52), anoise = f32(s + 56);
	bool changed;

	if (s[0] != PROTO)
		return;
	p->proto = s[0];
	if (id) {
		memcpy(name, s + 4, 16);
		name[16] = '\0';
		for (char *c = name; *c; c++)
			if (*c < ' ' || *c > '~')
				*c = '?';
	}
	/* a channel needs its units */
	if (!(gs > 0))
		ch &= (uint8_t)~1;
	if (!(as > 0))
		ch &= (uint8_t)~2;
	if (!id || !u32(s + 24)) {
		id = 0;
		ch = 0;
		name[0] = '\0';
	}
	if (!(noise > 0 && noise < 10))
		noise = 0;
	if (!(anoise > 0 && anoise < 1))
		anoise = 0;
	/* (a channel's scale only if it has the channel: as kept) */
	changed = !p->have_info || id != p->imu_id || strcmp(name, p->name) || ch != p->channels ||
		  u32(s + 24) != p->odr_mhz ||
		  (ch & 1 ? gs : 0) != p->gyro_scale || (ch & 2 ? as : 0) != p->accel_scale ||
		  noise != p->gyro_noise || anoise != p->accel_noise;
	if (changed && id)
		LOGI(TAG, "sensor %s: %.1f Hz,%s%s%s%s, noise %.3f deg/s %.4f g", name,
		     u32(s + 24) / 1000.0, ch & 1 ? " gyro" : "", ch & 2 ? " accelerometer" : "",
		     ch & 4 ? " magnetometer" : "", ch & 3 ? "" : " nothing to aim with",
		     (double)noise, (double)anoise);
	else if (changed)
		LOGI(TAG, "no sensor on the device");
	if (changed)
		new_stream(p, id ? u32(s + 24) : 0);
	p->imu_id = id;
	p->odr_mhz = u32(s + 24);
	memcpy(p->name, name, sizeof(p->name));
	p->channels = ch;
	p->gyro_scale = ch & 1 ? gs : 0;
	p->accel_scale = ch & 2 ? as : 0;
	p->gyro_noise = noise;
	p->accel_noise = anoise;
	p->have_info = true;
	if (changed) {
		p->info_seq++;
		if (id)
			identify(p);
	}
}

static int cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y;
}

/* The window's time and samples. The times are the sensor's grid and a
 * delay (the firmware's polling, up to ~0.35 ms): an interval is off its
 * samples' time by the difference of its two times' delays. Against r, the
 * mean of the middle half of the intervals' periods (not one of two
 * alternating groups, as their median can be), an interval counts if off
 * by JITTER_US at most; else it holds a pause (a data-ready missed is a
 * period with no sample), a time found later than that (with the interval
 * after it), or another rate than r's: out. The jitter adds up to the
 * delay's spread over the window; none counting (the rate changed right
 * in it): no measure */
static void window_sums(const struct imu_parser *p, double *el_out, unsigned *cnt_out)
{
	float v[SPAN_MAX], r = 0;
	unsigned n = p->span_n, lo = n / 4, hi = n - n / 4, cnt = 0;
	double el = 0;

	for (unsigned i = 0; i < n; i++)
		v[i] = p->span_el[i] / p->span_cnt[i];
	qsort(v, n, sizeof(v[0]), cmp_float);
	for (unsigned i = lo; i < hi; i++)
		r += v[i];
	r /= (float)(hi - lo);
	for (unsigned i = 0; i < n; i++)
		if (fabsf(p->span_el[i] - p->span_cnt[i] * r) <= JITTER_US) {
			el += p->span_el[i];
			cnt += p->span_cnt[i];
		}
	*el_out = el;
	*cnt_out = cnt;
}

/* A window more to the period: of its rate (within RATE_SAME), with the
 * last RATE_WINDOWS - 1 ones (the spread of the times' delay over 2 s, not
 * over one window); else the period starts anew from it. The period is all
 * their time over all their samples */
static float add_window(struct imu_parser *p, double el, unsigned cnt)
{
	double sum_el = 0, sum_cnt = 0;

	for (unsigned i = 0; i < p->win_n; i++) {
		sum_el += p->win_el[i];
		sum_cnt += p->win_cnt[i];
	}
	if (!p->win_n || fabs(el / cnt - sum_el / sum_cnt) > RATE_SAME * (sum_el / sum_cnt))
		p->win_n = p->win_head = 0;	/* another rate: from this window on */
	p->win_el[p->win_head] = el;
	p->win_cnt[p->win_head] = cnt;
	p->win_head = (p->win_head + 1) % RATE_WINDOWS;
	if (p->win_n < RATE_WINDOWS)
		p->win_n++;
	sum_el = sum_cnt = 0;
	for (unsigned i = 0; i < p->win_n; i++) {
		sum_el += p->win_el[i];
		sum_cnt += p->win_cnt[i];
	}
	return (float)(sum_el / sum_cnt);
}

/* A measure: taken within 0.5..2x of the info's rate, else the info's again
 * (and the measure kept to be told) */
static void take_rate(struct imu_parser *p, float m)
{
	if (m >= p->nominal_us / RATE_SLOWEST && m <= RATE_SLOWEST * p->nominal_us) {
		p->period_us = m;	/* trimmed: no further smoothing */
		p->period_measured = true;
		p->period_refused = 0;
	} else {
		p->period_us = p->nominal_us;
		p->period_measured = false;
		p->period_refused = m;
	}
}

/*
 * The real period, from the samples' own times: each interval between two
 * timed samples (its time, its n numbers: lost ones too) goes into a window
 * of SPAN_US and SPAN_MIN intervals at least (window_sums()); the windows
 * make the period (add_window()). An interval adds no more to the window's
 * time than WINDOW_MOST periods (the one in use) for each of its samples
 * that came: a window with a pause or lost samples in it is as long (and
 * one at half the period in use is not slowed). A rate off the info's after the
 * plug, or changed by more than RATE_SAME without an info, counts within
 * two windows; a smaller change over RATE_WINDOWS.
 */
static void measure_rate(struct imu_parser *p, float elapsed, unsigned n, unsigned came)
{
	float most = came * p->period_us * WINDOW_MOST;

	if (p->span_n < SPAN_MAX) {
		p->span_el[p->span_n] = elapsed;
		p->span_cnt[p->span_n] = (uint16_t)n;
		p->span_n++;
	}
	p->span_us += (uint64_t)(elapsed < most ? elapsed : most);
	if (p->span_us < SPAN_US || p->span_n < SPAN_MIN)
		return;
	{
		double el;
		unsigned cnt;

		window_sums(p, &el, &cnt);
		if (cnt)
			take_rate(p, add_window(p, el, cnt));
	}
	p->span_us = 0;
	p->span_n = 0;
}

/*
 * One sample; 'timed': t is its own time (not made up from the period).
 *
 * Gaps are decided here and only here: the first sample of a stream (a new
 * port, a changed info), or more than GAP_MISSING samples missing (and a half,
 * for the timing's jitter), counting together those missing by number and
 * those missing by time with the numbers going on (the device set its
 * sensor up again). A gap tells core/input (input_motion_gap()) and the
 * sample counts one period: what happened in the gap is not known. Else the
 * sample counts the steps its number made, GAP_MISSING + 1 periods at most.
 */
static void on_sample(struct imu_parser *p, uint32_t t, uint16_t seq, const uint8_t *gyro_raw,
		      const uint8_t *accel_raw, bool timed)
{
	float accel[3], gyro[3];
	float missing = 0;	/* samples missing before this one: by number, then by time */
	uint16_t steps = 1;
	bool gap = !p->have_prev;
	int k;

	for (k = 0; k < 3; k++) {
		gyro[k] = s16(gyro_raw + 2 * k) * p->gyro_scale;
		accel[k] = s16(accel_raw + 2 * k) * p->accel_scale;
	}
	if (p->have_prev) {
		steps = (uint16_t)(seq - p->prev_seq);
		missing = steps - 1;
	}
	if (timed && p->have_real) {
		uint16_t n = (uint16_t)(seq - p->real_seq);
		float expected = n * p->period_us, elapsed = (float)(uint32_t)(t - p->real_t);
		/* time beyond what the numbers say: more missing samples. With
		 * no measure (none yet, or refused) the info's period may be off
		 * by up to RATE_SLOWEST: only the time beyond that counts, in
		 * such periods (a whole batch at half the info's rate is no
		 * pause) */
		float slow = p->period_measured ? 1 : RATE_SLOWEST;

		if (elapsed > expected * slow)
			missing += (elapsed - expected * slow) / (p->period_us * slow);
		if (n >= 1)
			measure_rate(p, elapsed, n, p->since_real);
	}
	if (missing > GAP_MISSING + 0.5f)
		gap = true;
	if (gap) {
		input_motion_gap();
		steps = 1;
	}
	if (timed) {
		p->have_real = true;
		p->real_t = t;
		p->real_seq = seq;
		p->since_real = 0;
	}
	p->since_real++;
	p->have_prev = true;
	p->prev_seq = seq;
	p->samples++;
	input_report_motion(gyro, accel, steps * p->period_us * 1e-6f);
}

static void on_frame(struct imu_parser *p, uint8_t type, const uint8_t *f, uint8_t len)
{
	if (type == PKT_STATUS && len == STATUS_LEN) {
		on_status(p, f);
	} else if (type == PKT_INFO && len == INFO_LEN) {
		on_info(p, f);
	} else if (type == PKT_INFO) {
		/* another layout: the firmware is older or newer than this */
		if (p->bad_info != len)
			LOGW(TAG, "sensor info of %u bytes, %u expected: update the sensor's firmware "
			     "or bare_app", len, INFO_LEN);
		p->bad_info = len;
	} else if (type == PKT_SAMPLES && len >= SAMPLES_HDR && p->have_info && p->imu_id &&
		   p->proto == PROTO &&
		   len == SAMPLES_HDR + SAMPLE_LEN * f[6]) {
		/* the last frame again (its first number the last frame's) */
		if (p->have_prev && u16(f + 4) == p->frame_seq)
			return;
		p->batch_bytes = (size_t)len + FRAME_OVERHEAD;
		p->frame_seq = u16(f + 4);
		for (int i = 0; i < f[6]; i++) {
			const uint8_t *s = f + SAMPLES_HDR + SAMPLE_LEN * i;

			on_sample(p, u32(f), (uint16_t)(u16(f + 4) + i), s, s + 6, i == 0);
		}
	}
	/* PKT_MAG: not used for aiming; 0x01: protocol 2's samples, not read */
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
			uint8_t *f = p->buf + i, len = f[3];

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
			on_frame(p, f[2], f + 4, len);
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
	char serial[33];	/* the USB device's, "" none */
	bool live;		/* samples came lately (LIVE_MS, or 3 of its periods) */
	uint64_t last_ms;	/* now_ms() of the last read that gave samples */
	struct imu_rate_told rate;	/* logged of the rate */
	size_t vmin;		/* the port's VMIN now */
	bool told_zero;		/* logged: the gyro's zero */
	struct uevent_mon *mon;
	struct ml_timer *retry, *watch;
	int retries;
};

static void report_present(struct imu *h)
{
	bool ok = h->fd >= 0 && h->live;

	input_motion_present(ok, ok ? 1e6f / h->parser.period_us : 0);
}

/* poll() wakes for a whole batch of samples (VMIN: as long as the device's
 * PKT_SAMPLES frames are), 1 while no samples come */
static void set_vmin(struct imu *h, size_t bytes)
{
	struct termios tio;

	if (bytes < 1)
		bytes = 1;
	if (bytes > 255)
		bytes = 255;
	if (bytes == h->vmin || tcgetattr(h->fd, &tio))
		return;
	tio.c_cc[VMIN] = (cc_t)bytes;
	tio.c_cc[VTIME] = 0;
	if (tcsetattr(h->fd, TCSANOW, &tio) == 0)
		h->vmin = bytes;
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
	uint32_t had = h->parser.samples;

	while ((n = read(fd, buf, sizeof(buf))) > 0)
		imu_parser_feed(&h->parser, buf, (size_t)n);
	/* samples flowing: wake for a batch of them */
	if (n != 0 && h->parser.samples != had && h->parser.batch_bytes != h->vmin)
		set_vmin(h, h->parser.batch_bytes);
	if (h->parser.samples != had)
		h->last_ms = now_ms();
	if (!h->live && h->parser.samples != had) {
		h->live = true;
		report_present(h);
	}
	if ((n < 0 && errno != EAGAIN) || n == 0 || (revents & (POLLERR | POLLHUP)))
		detach(h);	/* unplugged */
}

bool imu_live(uint64_t now, uint64_t last, uint32_t samples, float period_us)
{
	return samples && now - last <= fmaxf(LIVE_MS, 3 * period_us / 1000);
}

bool imu_watch(bool *live, uint64_t now, uint64_t last, uint32_t samples, float period_us)
{
	bool was = *live;

	*live = imu_live(now, last, samples, period_us);
	return was && !*live;	/* (the old one: said every second while gone) */
}

int imu_rate_news(const struct imu_parser *p, struct imu_rate_told *t)
{
	int now = p->period_measured ? IMU_RATE_MEASURED : p->period_refused > 0 ? IMU_RATE_REFUSED : 0;

	if (t->info_seq != p->info_seq) {
		t->info_seq = p->info_seq;
		t->told = 0;	/* a new rate: told anew */
	}
	if (!now || now == t->told)
		return 0;
	t->told = now;
	return now;
}

/* Every second: the rate measured, and the stream asked for if it stopped
 * (no samples for LIVE_MS or 3 of the sensor's periods, whichever is longer) */
static void on_watch(void *ud)
{
	struct imu *h = ud;

	if (h->fd < 0)
		return;
	/* a slow sensor's samples are seconds apart: not "none" for that */
	if (imu_watch(&h->live, now_ms(), h->last_ms, h->parser.samples, h->parser.period_us))
		LOGW(TAG, "%s: no samples", h->node);	/* once, when they stop */
	if (!h->live)
		send_cmd(h, "s");
	if (!h->live)
		set_vmin(h, 1);	/* no samples: a status alone wakes too */
	report_present(h);
	switch (imu_rate_news(&h->parser, &h->rate)) {
	case IMU_RATE_MEASURED:
		LOGI(TAG, "%s: %.1f samples a second", h->node, 1e6 / h->parser.period_us);
		break;
	case IMU_RATE_REFUSED:
		LOGW(TAG, "%s: %.1f samples a second, beyond 0.5..2x of the info's %.1f: the info's kept",
		     h->node, 1e6 / h->parser.period_refused, 1e6 / h->parser.nominal_us);
		break;
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
		tio.c_cc[VMIN] = 1;
		tio.c_cc[VTIME] = 0;
		tcsetattr(fd, TCSANOW, &tio);
	}
	tcflush(fd, TCIFLUSH);
	h->fd = fd;
	memcpy(h->node, path, sizeof(h->node));
	imu_parser_init(&h->parser);
	memcpy(h->parser.serial, h->serial, sizeof(h->parser.serial));
	h->vmin = 1;
	h->live = false;
	h->rate = (struct imu_rate_told){ 0 };
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
		if (!strncmp(e->d_name, "ttyACM", 6) && is_ours(h, e->d_name)) {
			char dir[320];

			/* the USB serial: the sensor's calibration is its own */
			snprintf(dir, sizeof(dir), "/sys/class/tty/%.256s/device/..", e->d_name);
			if (!read_attr(dir, "serial", h->serial, sizeof(h->serial)))
				h->serial[0] = '\0';
			for (char *c = h->serial; *c; c++)
				if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') ||
				      (*c >= 'a' && *c <= 'z')))
					*c = '_';
			found = open_port(h, e->d_name);
		}
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
