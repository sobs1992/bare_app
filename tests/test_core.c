/* Host unit tests for the toolkit independent core. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/frametimes.h"
#include "core/remap.h"
#include "stream/rtp.h"
#include "stream/ts.h"
#include "core/input.h"
#include "core/joystick.h"
#include "core/mainloop.h"
#include "core/motion.h"
#include "core/msgq.h"
#include "core/uevent.h"
#include "core/volume.h"
#include "drivers/blit.h"
#include "media/mailbox.h"
#include "gui/action.h"
#include "gui/control/control.h"
#include "gui/ctx.h"
#include "services/config.h"
#ifdef CONFIG_SYSMON
#include <sys/stat.h>
#include "services/sysmon.h"
#endif
#if defined(CONFIG_WIFI) && defined(CONFIG_PLATFORM_OGA)
#include "drivers/wifi_wpa.h"
#endif
#ifdef CONFIG_INPUT_IMU
#include "drivers/input_imu.h"
#endif

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

/* A stick with a square gate, off center, each half axis its own reach:
 * the farthest point it reaches at angle a (radians) */
static void square_stick(float a, float *x, float *y)
{
	float cx = cosf(a), cy = sinf(a), m = fmaxf(fabsf(cx), fabsf(cy));

	cx /= m;
	cy /= m;
	*x = 0.1f + cx * (cx < 0 ? 0.6f : 0.8f);
	*y = -0.05f + cy * (cy < 0 ? 0.9f : 0.7f);
}

static void test_joystick(void)
{
	/* OGA rev 1.1 X axis: 172..772 */
	struct joy_axis_cal c = { .min = 172, .max = 772 };
	struct joy_stick_cal sc, back;
	struct joy_cal_rec rec;
	char buf[400];
	float x, y, a;
	int i;

	CHECK(NEAR(joy_linear(&c, 472), 0));
	CHECK(NEAR(joy_linear(&c, 772), 1));
	CHECK(NEAR(joy_linear(&c, 172), -1));
	CHECK(NEAR(joy_linear(&c, 900), 1));		/* clamped */
	CHECK(NEAR(joy_linear(&c, 622), 0.5f));
	c.invert = true;
	CHECK(NEAR(joy_linear(&c, 772), -1));

	/* no calibration: from the rest position to the range's ends */
	joy_stick_default(&sc, 0.2f, 0);
	joy_stick_apply(&sc, 0.1f, 0.2f, 0, &x, &y);
	CHECK(NEAR(x, 0) && NEAR(y, 0));
	joy_stick_apply(&sc, 0.1f, 1, 0, &x, &y);
	CHECK(NEAR(x, 1) && NEAR(y, 0));
	joy_stick_apply(&sc, 0.1f, -1, 0, &x, &y);
	CHECK(NEAR(x, -1));
	joy_stick_apply(&sc, 0.1f, 0.25f, 0.05f, &x, &y);	/* round dead zone */
	CHECK(NEAR(x, 0) && NEAR(y, 0));
	joy_stick_apply(&sc, 0.1f, 0.2f, 0.5f, &x, &y);	/* half way down */
	CHECK(NEAR(x, 0) && NEAR(y, 0.4f / 0.9f));

	/* calibration: rest position, then turned along the edge twice */
	joy_cal_begin(&rec, 0.1f, -0.05f);
	CHECK(joy_cal_covered(&rec) == 0);
	for (i = 0; i < 60; i++) {
		square_stick((float)i * 0.07f, &x, &y);
		joy_cal_add(&rec, x, y);
	}
	CHECK(joy_cal_covered(&rec) < JOY_SECTORS);	/* not all the way round yet */
	CHECK(!joy_cal_finish(&rec, &sc));
	for (; i < 200; i++) {
		square_stick((float)i * 0.07f, &x, &y);
		joy_cal_add(&rec, x, y);
	}
	CHECK(joy_cal_covered(&rec) == JOY_SECTORS);
	CHECK(joy_cal_finish(&rec, &sc));
	/* the edge is now the unit circle everywhere, the rest position 0 */
	for (a = 0; a < 6.28f; a += 0.1f) {
		square_stick(a, &x, &y);
		joy_stick_apply(&sc, 0.15f, x, y, &x, &y);
		CHECK(fabsf(hypotf(x, y) - 1) < 0.02f);
	}
	joy_stick_apply(&sc, 0.15f, 0.1f, -0.05f, &x, &y);
	CHECK(NEAR(x, 0) && NEAR(y, 0));
	/* straight right and straight up stay so */
	joy_stick_apply(&sc, 0.15f, 0.9f, -0.05f, &x, &y);
	CHECK(NEAR(x, 1) && NEAR(y, 0));
	joy_stick_apply(&sc, 0.15f, 0.1f, -0.95f, &x, &y);
	CHECK(NEAR(x, 0) && NEAR(y, -1));

	/* sensitivity: scaled, cut at the edge in the same direction */
	x = 0.3f, y = -0.4f;
	joy_stick_gain(1.5f, &x, &y);
	CHECK(NEAR(x, 0.45f) && NEAR(y, -0.6f));
	x = 0.6f, y = 0.8f;
	joy_stick_gain(2, &x, &y);
	CHECK(NEAR(x, 0.6f) && NEAR(y, 0.8f));
	x = 1, y = 0;
	joy_stick_gain(0.5f, &x, &y);
	CHECK(NEAR(x, 0.5f) && NEAR(y, 0));

	/* snap-back, a reading every 10 ms */
	{
		struct joy_snap sn = { 0 };
		unsigned long long t = 0;
		int i;
		bool held = false;

		/* held down, let go: 1 -> 0.3 in 40 ms, then creeping to 0 */
		for (i = 0; i < 20; i++, t += 10)
			CHECK(!joy_snap_update(&sn, t, 1));
		CHECK(!joy_snap_update(&sn, t += 10, 0.8f));
		CHECK(!joy_snap_update(&sn, t += 10, 0.6f));
		joy_snap_update(&sn, t += 10, 0.45f);
		CHECK(joy_snap_update(&sn, t += 10, 0.3f));	/* fell fast: at rest */
		for (i = 0; i < 100; i++)			/* the slow creep stays at rest */
			CHECK(joy_snap_update(&sn, t += 10, 0.3f - 0.003f * (float)i));
		CHECK(joy_snap_update(&sn, t += 10, 0.05f));	/* noise near the low */
		CHECK(!joy_snap_update(&sn, t += 10, 0.5f));	/* pushed out again */

		/* brought back slowly by hand: never "at rest" early */
		for (i = 0; i < 20; i++, t += 10)
			joy_snap_update(&sn, t, 1);
		for (i = 0; i <= 100; i++)
			held |= joy_snap_update(&sn, t += 10, 1 - 0.01f * (float)i);
		CHECK(!held);

		/* flicked across: at rest a moment, then the other side counts */
		for (i = 0; i < 20; i++, t += 10)
			joy_snap_update(&sn, t, 1);
		CHECK(joy_snap_update(&sn, t += 10, 0.1f));
		CHECK(!joy_snap_update(&sn, t += 10, 0.9f));
	}
	x = y = 0;
	joy_stick_gain(2, &x, &y);
	CHECK(x == 0 && y == 0);

	/* settings file text */
	CHECK(joy_stick_format(&sc, buf, sizeof(buf)) > 0);
	CHECK(joy_stick_parse(buf, &back));
	CHECK(NEAR(back.cx, sc.cx) && NEAR(back.yp, sc.yp) && NEAR(back.edge[5], sc.edge[5]));
	CHECK(!joy_stick_parse("0 0 1 1 1", &back));
	CHECK(!joy_stick_parse("", &back));

	CHECK(joy_direction(0, 0.6f, 0.5f, 0.3f) == 1);
	CHECK(joy_direction(1, 0.4f, 0.5f, 0.3f) == 1);		/* hysteresis holds */
	CHECK(joy_direction(1, 0.2f, 0.5f, 0.3f) == 0);
	CHECK(joy_direction(0, 0.4f, 0.5f, 0.3f) == 0);
	CHECK(joy_direction(1, -0.6f, 0.5f, 0.3f) == -1);	/* flick to the other side */
	CHECK(joy_direction(-1, -0.4f, 0.5f, 0.3f) == -1);
}

static void test_frametimes(void)
{
	static struct frametimes f;
	uint64_t t[FRAMETIMES_MAX];
	struct frametimes_summary s;
	unsigned n, i;
	uint64_t us = 1000000;

	frametimes_reset(&f);
	CHECK(frametimes_last(&f, t, FRAMETIMES_MAX) == 0);
	frametimes_summarize(t, 0, 0, 0, &s);
	CHECK(s.frames == 0 && s.fps_x10 == -1);
	/* 2000 frames at 30 fps (33.333 ms), one 200 ms stall, the ring keeps the last 1024 */
	for (i = 0; i < 2000; i++) {
		us += i == 1990 ? 200000 : 33333;
		frametimes_add(&f, us);
	}
	n = frametimes_last(&f, t, FRAMETIMES_MAX);
	CHECK(n == FRAMETIMES_MAX);
	CHECK(t[n - 1] == us);
	for (i = 1; i < n; i++)
		CHECK(t[i] > t[i - 1]);
	frametimes_summarize(t, n, us - 1000000, 0, &s);	/* the last second */
	CHECK(s.frames >= 25 && s.frames <= 27);
	CHECK(s.median_us == 33333);
	CHECK(s.worst_us == 200000);
	CHECK(s.hitches == 1);
	CHECK(s.fps_x10 >= 240 && s.fps_x10 <= 270);
	/* the same with 200 ms as a pause (a still picture): no hitch, 30 fps */
	frametimes_summarize(t, n, us - 1000000, 150000, &s);
	CHECK(s.idle_us == 200000);
	CHECK(s.hitches == 0 && s.worst_us == 33333);
	CHECK(s.fps_x10 >= 299 && s.fps_x10 <= 301);
	CHECK(frametimes_is_hitch(70000, 33333));
	CHECK(!frametimes_is_hitch(20000, 5000));	/* short ones never are */
	n = frametimes_last(&f, t, 10);
	CHECK(n == 10 && t[9] == us);
}

static void test_remap(void)
{
	struct gp_remap m;
	char buf[64];

	gp_remap_reset(&m);
	CHECK(gp_remap_is_default(&m));
	CHECK(gp_remap_format(&m, buf, sizeof(buf)) && !strcmp(buf, ""));
	CHECK(gp_remap_parse(&m, "a=b,b=a,x=-") == 0);
	CHECK(gp_remap_apply(&m, GP_A) == GP_B && gp_remap_apply(&m, GP_B) == GP_A);
	CHECK(gp_remap_apply(&m, GP_X) == GP_NONE && gp_remap_apply(&m, GP_Y) == GP_Y);
	CHECK(gp_remap_format(&m, buf, sizeof(buf)) && !strcmp(buf, "a=b,b=a,x=-"));
	CHECK(!gp_remap_format(&m, buf, 8));	/* too small: cut at a pair */
	CHECK(!strcmp(buf, "a=b,b=a"));
	/* unknown names are skipped, the rest is kept */
	CHECK(gp_remap_parse(&m, "a=zz,qq=b,l1=r1,") == -1);
	CHECK(gp_remap_apply(&m, GP_A) == GP_A && gp_remap_apply(&m, GP_L1) == GP_R1);
	CHECK(gp_remap_parse(&m, NULL) == 0 && gp_remap_is_default(&m));
	/* inverted axes: the value the other way round, the left stick's
	 * directions swapped with them */
	CHECK(gp_remap_parse(&m, "a=b,ly=invert,rx=invert") == 0 && !gp_remap_is_default(&m));
	CHECK(m.invert == (GP_BIT(GP_AXIS_LY) | GP_BIT(GP_AXIS_RX)));
	CHECK(gp_remap_axis(&m, GP_AXIS_LY, 0.5f) == -0.5f && gp_remap_axis(&m, GP_AXIS_LX, 0.5f) == 0.5f);
	CHECK(gp_remap_axis(&m, GP_AXIS_RX, -1) == 1 && gp_remap_axis(&m, GP_AXIS_RY, -1) == -1);
	CHECK(gp_remap_stick_dir(&m, GP_UP) == GP_DOWN && gp_remap_stick_dir(&m, GP_DOWN) == GP_UP);
	CHECK(gp_remap_stick_dir(&m, GP_LEFT) == GP_LEFT && gp_remap_stick_dir(&m, GP_A) == GP_A);
	CHECK(gp_remap_format(&m, buf, sizeof(buf)) && !strcmp(buf, "a=b,ly=invert,rx=invert"));
	CHECK(gp_remap_parse(&m, "lx=invert") == 0 && gp_remap_stick_dir(&m, GP_RIGHT) == GP_LEFT);
	CHECK(gp_remap_parse(&m, "ly=on,ry=invert") == -1 && m.invert == GP_BIT(GP_AXIS_RY));
	m.invert = 0;
	CHECK(gp_remap_is_default(&m));
	/* the motion sensor: on, off or the game's; its sensitivity in tenths */
	CHECK(!gp_remap_gyro(&m, false) && gp_remap_gyro(&m, true));
	CHECK(gp_remap_gyro_sens(&m) == GP_GYRO_SENS_DEFAULT / 10.0f);
	CHECK(gp_remap_gyro_sens(NULL) == GP_GYRO_SENS_DEFAULT / 10.0f && gp_remap_gyro(NULL, true));
	CHECK(gp_remap_parse(&m, "gyro=on,gyrosens=3.5") == 0 && !gp_remap_is_default(&m));
	CHECK(gp_remap_gyro(&m, false) && m.gyro_sens == 35);
	CHECK(gp_remap_format(&m, buf, sizeof(buf)) && !strcmp(buf, "gyro=on,gyrosens=3.5"));
	CHECK(gp_remap_parse(&m, "gyro=off") == 0 && !gp_remap_gyro(&m, true));
	CHECK(gp_remap_format(&m, buf, sizeof(buf)) && !strcmp(buf, "gyro=off"));
	CHECK(gp_remap_parse(&m, "gyro=maybe,gyrosens=99,gyrosens=x") == -1 && gp_remap_is_default(&m));
}

/* n samples of a turn (deg/s) at 1 kHz, the accelerometer at 'up' */
static void motion_turn(struct motion *m, float wx, float wy, float wz, const float up[3], int n)
{
	const float bias[3] = { 0.4f, -0.2f, 0.1f };

	for (int i = 0; i < n; i++) {
		float g[3] = { wx + bias[0], wy + bias[1], wz + bias[2] };

		/* the noise of the real sensor, about 0.15 deg/s */
		g[0] += (float)((i * 7919 % 13) - 6) * 0.04f;
		g[1] += (float)((i * 104729 % 11) - 5) * 0.04f;
		motion_feed(m, g, up, 0.001f);
	}
}

static void test_motion(void)
{
	const float z_up[3] = { 0, 0, 1 }, y_up[3] = { 0, 1, 0 };
	struct motion_cal c, c2;
	struct motion m;
	struct motion_rec r;
	float x, y, right[3], bias[3];
	char buf[128];

	/* lying still: the bias is measured, nothing is turned */
	motion_init(&m);
	motion_turn(&m, 0, 0, 0, z_up, 900);
	CHECK(!m.bias_known);
	motion_turn(&m, 0, 0, 0, z_up, 200);
	CHECK(m.bias_known && m.still && fabsf(m.bias[0] - 0.4f) < 0.02f && fabsf(m.bias[2] - 0.1f) < 0.02f);
	motion_take(&m, &x, &y);
	motion_turn(&m, 0, 0, 0, z_up, 500);
	motion_take(&m, &x, &y);
	CHECK(fabsf(x) < 0.05f && y == 0);

	/* turned left about "up" by 30 degrees: x goes left; no up and down
	 * without the calibration */
	motion_turn(&m, 0, 0, 60, z_up, 500);
	motion_take(&m, &x, &y);
	CHECK(fabsf(x + 30) < 1 && y == 0);
	/* the same with the device upright (the sensor's y up) */
	motion_init(&m);
	motion_turn(&m, 0, 0, 0, y_up, 1100);
	motion_turn(&m, 0, -40, 0, y_up, 500);
	motion_take(&m, &x, &y);
	CHECK(fabsf(x - 20) < 1);
	/* a slow steady turn of a device lying still is not a new zero */
	motion_turn(&m, 0, 3, 0, y_up, 1100);
	CHECK(fabsf(m.bias[1] + 0.2f) < 0.05f);

	/* calibrated right = sensor x: the top towards the player looks down */
	c = (struct motion_cal){ .axes_set = true, .right = { 2, 0, 0 } };
	motion_set_cal(&m, &c);
	CHECK(m.cal.axes_set && m.cal.right[0] == 1);
	motion_take(&m, &x, &y);
	motion_turn(&m, 20, 0, 0, y_up, 500);
	motion_take(&m, &x, &y);
	CHECK(fabsf(y - 10) < 1 && fabsf(x) < 0.5f);

	/* small turns are smoothed, not lost */
	motion_take(&m, &x, &y);
	motion_turn(&m, 0, 1, 0, y_up, 1000);
	motion_turn(&m, 0, 0, 0, y_up, 100);
	motion_take(&m, &x, &y);
	CHECK(fabsf(x + 1) < 0.15f);

	/* the calibration: still, then a tilt about the sensor's x */
	motion_rec_begin(&r);
	CHECK(motion_rec_still(&r, (float[]){ 0.4f, -0.2f, 0.1f }, z_up));
	CHECK(motion_rec_still(&r, (float[]){ 0.5f, -0.1f, 0.1f }, z_up));
	CHECK(!motion_rec_still(&r, (float[]){ 9, 0, 0 }, z_up));
	CHECK(motion_rec_bias(&r, bias) && fabsf(bias[0] - 9.9f / 3) < 1e-3f);
	motion_rec_tilt_begin(&r, y_up);
	for (int i = 0; i < 1000; i++)
		x = motion_rec_tilt(&r, (float[]){ 30.4f, 2, 0.1f }, (float[]){ 0.4f, 0, 0.1f }, 0.001f);
	CHECK(fabsf(x - 30) < 0.01f);
	CHECK(motion_rec_right(&r, right) && fabsf(right[0] - 1) < 1e-3f);
	motion_rec_tilt_begin(&r, y_up);
	for (int i = 0; i < 1000; i++)
		motion_rec_tilt(&r, (float[]){ 2, 40, 0 }, (float[]){ 0, 0, 0 }, 0.001f);
	CHECK(!motion_rec_right(&r, right));	/* turned about "up" */

	/* kept as text */
	c = (struct motion_cal){ .bias_set = true, .bias = { 0.5f, -1, 0 }, .axes_set = true,
				 .right = { 0, 0, -1 } };
	CHECK(motion_cal_format(&c, buf, sizeof(buf)) > 0);
	CHECK(!strcmp(buf, "bias 0.5000 -1.0000 0.0000 right 0.0000 0.0000 -1.0000"));
	CHECK(motion_cal_parse(buf, &c2) && c2.bias_set && c2.axes_set && c2.bias[1] == -1 &&
	      c2.right[2] == -1);
	CHECK(motion_cal_parse(" right 0 3 0\n", &c2) && !c2.bias_set && c2.right[1] == 1);
	CHECK(!motion_cal_parse("bias 1 2", &c2) && !motion_cal_parse("right 0 0 0", &c2) &&
	      !motion_cal_parse("left 1 0 0", &c2));
	CHECK(motion_cal_format(&(struct motion_cal){ 0 }, buf, sizeof(buf)) == 0 && !buf[0]);

	/* no gyro: one made from a noisy accelerometer (0.0022 g rms at 1125 Hz,
	 * the span from that noise) must be quiet enough at rest for the
	 * calibration's "held steady" (under 8 deg/s) and the thresholds, and its
	 * tilt must give the right axis */
	{
		static struct motion_synth sy;
		float est = motion_synth_setup(&sy, 0.0022f);
		struct motion_rec r;
		const float dt = 1 / 1125.0f, bias0[3] = { 0, 0, 0 };
		unsigned rng = 1;
		double sq = 0;
		float peak = 0, deg = 0, axis[3];
		int n = 0;

		for (int i = 0; i < 4500; i++) {
			float a[3], g[3], th = i < 2250 ? 0 : fminf((i - 2250) * dt * 40, 25) * 0.017453293f;

			a[0] = 0;
			a[1] = sinf(th);
			a[2] = cosf(th);
			for (int k = 0; k < 3; k++) {
				float u = 0;	/* about normal: a sum of 12 uniforms */

				for (int j = 0; j < 12; j++) {
					rng = rng * 1664525u + 1013904223u;
					u += (rng >> 8) / 16777216.0f;
				}
				a[k] += (u - 6) * 0.0022f;
			}
			motion_synth_gyro(&sy, a, dt, g);
			if (i >= 200 && i < 2250) {	/* at rest */
				float s2 = g[0] * g[0] + g[1] * g[1] + g[2] * g[2];

				sq += s2;
				n++;
				peak = fmaxf(peak, sqrtf(s2));
			}
			if (i == 2250)
				motion_rec_tilt_begin(&r, (float[]){ 0, 0, 0 });
			if (i >= 2250)
				deg = motion_rec_tilt(&r, g, bias0, dt);
		}
		CHECK(fabsf(sy.span - 0.099f) < 0.001f && est > 0.2f && est < 0.35f);
		CHECK(sqrt(sq / n) < 0.5 && peak < 2);
		CHECK(fabsf(deg - 25) < 2);
		CHECK(motion_rec_right(&r, axis) && axis[0] > 0.99f);
	}
}

#ifdef CONFIG_INPUT_IMU
static float imu_turned, imu_my;	/* GP_EV_MOTION seen: x, y */
static int imu_events;

static uint32_t imu_devices_seen;
static int imu_devices_calls;

static void on_imu_devices(uint32_t devices)
{
	imu_devices_seen = devices;
	imu_devices_calls++;
}

static void on_imu_event(const struct gp_event *ev, void *ud)
{
	(void)ud;
	if (ev->type == GP_EV_MOTION) {
		imu_turned += ev->mx;
		imu_my += ev->my;
		imu_events++;
	}
}

static size_t imu_frame(uint8_t *out, uint8_t type, const uint8_t *payload, uint8_t len)
{
	uint16_t crc;

	out[0] = 0xA5;
	out[1] = 0x5A;
	out[2] = type;
	out[3] = len;
	memcpy(out + 4, payload, len);
	crc = imu_crc16(out + 2, 2u + len);
	out[4 + len] = (uint8_t)crc;
	out[5 + len] = (uint8_t)(crc >> 8);
	return len + 6u;
}

#define IMU_SAMPLE_FRAME 26	/* a PKT_SAMPLES of one sample */

/* One sample as PKT_SAMPLES: turning gz about z, 1 g on z */
static size_t imu_sample(uint8_t *out, uint32_t t, uint16_t seq, int16_t gz)
{
	uint8_t p[20] = { 0 };

	memcpy(p, &t, 4);
	memcpy(p + 4, &seq, 2);
	p[6] = 1;
	memcpy(p + 8 + 4, &gz, 2);
	p[8 + 10] = 0x00;
	p[8 + 11] = 0x20;	/* accel z = 8192: 1 g at 4 g full scale */
	return imu_frame(out, 0x04, p, 20);
}

/* PKT_INFO: a sensor as the device describes it (60 bytes) */
static size_t imu_info(uint8_t *out, const char *name, uint8_t id, uint8_t channels, uint32_t odr,
		       float gs, float as, float gnoise, float anoise)
{
	uint8_t info[60] = { 3, 0, 3, id };

	strncpy((char *)info + 4, name, 16);
	info[20] = channels;
	info[21] = 4;
	memcpy(info + 24, &odr, 4);
	memcpy(info + 28, &gs, 4);
	memcpy(info + 32, &as, 4);
	memcpy(info + 52, &gnoise, 4);
	memcpy(info + 56, &anoise, 4);
	return imu_frame(out, 0x03, info, 60);
}

/* A sensor at 1125 Hz, +-2000 deg/s, +-4 g, all channels, noise unknown */
static size_t imu_info_test(uint8_t *out)
{
	return imu_info(out, "TESTIMU", 7, 1 | 2 | 4 | 8, 1125000, 2000.0f / 32768, 4.0f / 32768, 0, 0);
}

/* n samples of a turn (deg/s) through input_report_motion() at 1 kHz, the
 * accelerometer turned by 'tilt' degrees about x as it goes (0: flat) */
static void imu_feed(int n, float wx, float wy, float wz, float tilt0, float tilt1)
{
	for (int i = 0; i < n; i++) {
		float th = (tilt0 + (tilt1 - tilt0) * (i + 1) / n) * 0.017453293f;
		float g[3] = { wx, wy, wz }, a[3] = { 0, sinf(th), cosf(th) };

		input_report_motion(g, a, 0.001f);
		if (i % 4 == 3)
			input_report_motion_end();
	}
	input_report_motion_end();
}

static void test_imu(void)
{
	static uint8_t stream[4000 * IMU_SAMPLE_FRAME + 256];
	uint8_t status[36] = { 3, 0, 2, 0x0b }, v2[36] = { 2, 0, 1, 0x0b }, v2imu[28] = { 0 };
	struct imu_parser p;
	struct mainloop *ml = mainloop_new("imu");
	size_t n = 0, off;
	int i;

	CHECK(imu_crc16((const uint8_t *)"123456789", 9) == 0x29B1);	/* CCITT-FALSE check value */
	input_init(ml, NULL, on_imu_event, NULL);
	n += imu_info_test(stream + n);
	n += imu_frame(stream + n, 0x02, status, 36);
	/* 2 s still at 1000 Hz (the sensor's clock, 1125 Hz nominal), then 1 s
	 * turning at 32.8 deg/s: raw 537 = 32.78 deg/s at 2000 deg/s full scale;
	 * one sample lost */
	for (i = 0; i < 3000; i++)
		if (i != 2500)
			n += imu_sample(stream + n, 1000u * i, (uint16_t)(i + 65000),
					i < 2000 ? 0 : 537);
	stream[n++] = 0xA5;	/* junk between frames */
	stream[n++] = 0x00;
	imu_parser_init(&p);
	for (off = 0; off < n; off += 77)	/* in pieces that cut frames */
		imu_parser_feed(&p, stream + off, n - off < 77 ? n - off : 77);
	CHECK(p.have_status && p.imu_ok && p.samples == 2999 && p.bad_crc == 0);
	CHECK(p.proto == 3 && !strcmp(p.name, "TESTIMU") && p.info_seq == 1 && p.channels == 15);
	CHECK(fabsf(p.period_us - 1000) < 1);	/* measured over 2 s of the sensor's clock */
	CHECK(p.batch_bytes == IMU_SAMPLE_FRAME);
	CHECK(imu_events > 0 && fabsf(imu_turned + 32.78f) < 1.5f);
	/* a frame with a bad CRC is skipped */
	n = imu_sample(stream, 3000000, (uint16_t)(3000 + 65000), 0);
	stream[10] ^= 1;
	imu_parser_feed(&p, stream, n);
	CHECK(p.bad_crc > 0 && p.samples == 2999);
	/* protocol v2 (firmware 0.1) is not read: its samples are dropped */
	n = imu_frame(stream, 0x02, v2, 36);
	v2imu[11] = 0x20;
	n += imu_frame(stream + n, 0x01, v2imu, 28);
	imu_parser_feed(&p, stream, n);
	CHECK(p.samples == 2999 && p.proto == 2 && !p.have_info);
	n = imu_info_test(stream);
	n += imu_frame(stream + n, 0x02, status, 36);
	imu_parser_feed(&p, stream, n);
	CHECK(p.proto == 3 && p.have_info);

	/* the tune for every game: sensitivity, inverted axes */
	{
		struct input_motion_tune t = { .sensitivity = 2, .invert_x = true }, got;
		FILE *f;

		input_set_motion_tune(&t);
		imu_turned = 0;
		for (i = 0; i < 1000; i++)
			n = imu_sample(stream + IMU_SAMPLE_FRAME * (size_t)i, 3001000u + 1000u * i,
				       (uint16_t)(i + 3001 + 65000), 537);
		imu_parser_feed(&p, stream, IMU_SAMPLE_FRAME * 1000);
		/* the period starts again from the nominal one after the v2 frames */
		CHECK(fabsf(imu_turned - 2 * 32.78f * p.period_us / 1000) < 3);
		input_set_motion_tune(&(struct input_motion_tune){ .sensitivity = 9 });
		input_motion_tune(&got);
		CHECK(got.sensitivity == INPUT_GYRO_SENS_MAX && !got.invert_x);
		input_set_motion_tune(NULL);
		input_motion_tune(&got);
		CHECK(got.sensitivity == 1 && !got.invert_x && !got.invert_y);
		/* kept with the calibration */
		f = fopen("test_gyro.conf", "w");
		fputs("gyro.sensitivity = 1.5\ngyro.invert_y = 1\ngyro.bogus = 1\n", f);
		fclose(f);
		CHECK(input_load_calibration("test_gyro.conf") == 0);
		input_motion_tune(&got);
		CHECK(got.sensitivity == 1.5f && got.invert_y && !got.invert_x);
		CHECK(input_save_calibration() == 0);
		input_set_motion_tune(NULL);
		CHECK(input_load_calibration("test_gyro.conf") == 0);
		input_motion_tune(&got);
		CHECK(got.sensitivity == 1.5f && got.invert_y);
		unlink("test_gyro.conf");
		input_set_motion_tune(NULL);
	}

	/* the sensor says its units and rate (unlike the one above: 833 Hz, 70
	 * mdps and 0.122 mg a count, noise 0.12 deg/s and 0.0006 g), batches of
	 * 3; core/input learns it from the info, before its samples */
	{
		uint8_t hdr[8 + 36], st3[36] = { 3, 0, 3, 0x1b };
		size_t m = 0;
		struct motion_cal c;
		FILE *f = fopen("test_cal.conf", "w");

		/* this sensor's calibration is waiting for it */
		fputs("gyro.FAKE-SER1 = right 0 1 0\n", f);
		fclose(f);
		CHECK(input_load_calibration("test_cal.conf") == 0);
		unlink("test_cal.conf");
		imu_parser_init(&p);
		snprintf(p.serial, sizeof(p.serial), "SER1");
		/* samples before the info: units unknown, dropped */
		memset(hdr, 0, sizeof(hdr));
		hdr[6] = 3;
		m += imu_frame(stream + m, 0x04, hdr, 8 + 36);
		m += imu_info(stream + m, "FAKE", 2, 1 | 2 | 8, 833000, 0.07f, 0.000122f, 0.12f, 0.0006f);
		m += imu_frame(stream + m, 0x02, st3, 36);
		imu_turned = 0;
		/* 3 s still, then 1 s at 45 deg/s about z (643 counts of 0.07) */
		for (i = 0; i < 4 * 833 / 3; i++) {
			uint32_t t = 3000000u + (uint32_t)((uint64_t)3 * i * 1000000000u / 833000);
			uint16_t sq = (uint16_t)(3 * i + 65000);
			int16_t gz = 3 * i >= 3 * 833 ? 643 : 0, az = 8197;	/* 1 g */

			memcpy(hdr, &t, 4);
			memcpy(hdr + 4, &sq, 2);
			hdr[6] = 3;
			for (int k = 0; k < 3; k++) {
				memset(hdr + 8 + 12 * k, 0, 12);
				memcpy(hdr + 8 + 12 * k + 4, &gz, 2);
				memcpy(hdr + 8 + 12 * k + 10, &az, 2);
			}
			m += imu_frame(stream + m, 0x04, hdr, 8 + 36);
		}
		for (off = 0; off < m; off += 61)
			imu_parser_feed(&p, stream + off, m - off < 61 ? m - off : 61);
		CHECK(p.proto == 3 && p.have_info && p.imu_id == 2 && !strcmp(p.name, "FAKE"));
		CHECK(p.info_seq == 1 && p.gyro_noise == 0.12f && p.accel_noise == 0.0006f);
		CHECK(p.channels == (1 | 2 | 8) && p.fake && p.imu_ok && p.batch_bytes == 50);
		CHECK(p.samples == 3 * (4 * 833 / 3) && p.bad_crc == 0);
		CHECK(p.period_measured && fabsf(p.period_us - 1e6f / 833) < 1);
		/* core/input: the sensor, its channels and noise, its calibration */
		CHECK(input_motion()->channels == (MOTION_CH_GYRO | MOTION_CH_ACCEL));
		CHECK(fabsf(input_motion()->still_gyro - 0.48f) < 1e-5f &&
		      input_motion()->still_accel == MOTION_STILL_ACCEL_MIN);	/* 4 x 0.0006, at least */
		CHECK(input_motion_calibration(&c) && c.axes_set && c.right[1] == 1);
		/* about 45 degrees to the left: x negative (the zero measured meanwhile) */
		CHECK(fabsf(imu_turned + 45) < 2.5f);
		/* a channel without units is not there */
		n = imu_info(stream, "FAKE", 2, 1 | 2 | 8, 833000, 0, 0.000122f, 0.12f, 0.0006f);
		imu_parser_feed(&p, stream, n);
		CHECK(p.channels == (2 | 8) && p.info_seq == 2 && input_motion()->channels == MOTION_CH_ACCEL);
		/* no sensor on the device: units forgotten, its samples dropped */
		n = imu_info(stream, "", 0, 0, 0, 0, 0, 0, 0);
		imu_parser_feed(&p, stream, n);
		CHECK(p.imu_id == 0 && !p.name[0] && !p.channels && p.info_seq == 3);
		input_set_motion_calibration(NULL);
	}

	/* each sensor its own calibration, by its id alone */
	{
		struct motion_cal c;
		FILE *f = fopen("test_cal.conf", "w");
		char text[1024] = "";

		fputs("gyro = bias 0.1 0.2 0.3 right -1 0 0\ngyro.FAKE-X1 = right 0 1 0\n", f);
		fclose(f);
		CHECK(input_load_calibration("test_cal.conf") == 0);
		/* no sensor named yet: no calibration; a line without a sensor is no one's */
		CHECK(!input_motion_calibration(&c) && !input_motion()->cal.axes_set);
		input_motion_sensor(&(struct input_motion_desc){ "AAA-1", "AAA", 3, 0.15f, 0.0022f });
		CHECK(!input_motion_calibration(&c));
		CHECK(fabsf(input_motion()->still_gyro - 0.6f) < 1e-5f &&
		      fabsf(input_motion()->still_accel - 0.0088f) < 1e-6f);
		CHECK(fabsf(input_motion()->smooth_dps - 3.75f) < 1e-5f &&
		      fabsf(input_motion()->tight_dps - 0.3f) < 1e-6f);
		input_motion_sensor(&(struct input_motion_desc){ "FAKE-X1", "FAKE", 3, 0.12f, 0 });
		CHECK(input_motion_calibration(&c) && c.right[1] == 1 && !c.bias_set);
		CHECK(!input_motion()->bias_known && input_motion()->still_accel == MOTION_STILL_ACCEL);
		input_motion_sensor(&(struct input_motion_desc){ "FAKE-X2", "FAKE", 3, 0, 0 });
		CHECK(!input_motion_calibration(&c) && input_motion()->still_gyro == MOTION_STILL_GYRO);
		CHECK(input_motion()->smooth_dps == MOTION_SMOOTH_DPS);
		input_set_motion_calibration(&(struct motion_cal){ .bias_set = true, .bias = { 1, 2, 3 } });
		input_motion_sensor(&(struct input_motion_desc){ "AAA-1", "AAA", 3, 0.15f, 0.0022f });
		input_set_motion_calibration(&(struct motion_cal){ .axes_set = true, .right = { 0, 0, 1 } });
		CHECK(input_save_calibration() == 0);
		f = fopen("test_cal.conf", "r");
		CHECK(f && fread(text, 1, sizeof(text) - 1, f) > 0);
		if (f)
			fclose(f);
		CHECK(strstr(text, "gyro.FAKE-X1 = right 0.0000 1.0000 0.0000\n"));
		CHECK(strstr(text, "gyro.FAKE-X2 = bias 1.0000 2.0000 3.0000\n"));
		CHECK(strstr(text, "gyro.AAA-1 = right 0.0000 0.0000 1.0000\n"));
		CHECK(!strstr(text, "\ngyro = "));
		CHECK(input_load_calibration("test_cal.conf") == 0);
		input_motion_sensor(&(struct input_motion_desc){ "FAKE-X2", "FAKE", 3, 0, 0 });
		CHECK(input_motion_calibration(&c) && c.bias[2] == 3);
		unlink("test_cal.conf");
		input_set_motion_calibration(NULL);
	}

	/* a sensor without an accelerometer: its own axes, from the calibration */
	{
		struct input_motion_info mi;
		struct motion_cal c = { .axes_set = true, .right = { 1, 0, 0 }, .up_set = true, .up = { 0, 0, 1 } };

		input_motion_sensor(&(struct input_motion_desc){ "GYRO-1", "GYRO", MOTION_CH_GYRO, 0.15f, 0 });
		input_motion_info(&mi);
		CHECK(mi.channels == MOTION_CH_GYRO && !mi.aim_x && !mi.aim_y && !mi.axes_set);
		imu_feed(1100, 0, 0, 0, 0, 0);	/* still: the zero, by the gyro alone */
		CHECK(input_motion()->bias_known);
		imu_turned = 0;
		imu_feed(500, 0, 0, 60, 0, 0);
		CHECK(imu_turned == 0);		/* no vertical known: nothing */
		input_set_motion_calibration(&c);
		input_motion_info(&mi);
		CHECK(mi.aim_x && mi.aim_y && mi.axes_set);
		imu_turned = 0;
		imu_feed(500, 0, 0, 60, 0, 0);	/* 30 degrees to the left about its vertical */
		CHECK(fabsf(imu_turned + 30) < 1);
		input_set_motion_calibration(NULL);
	}

	/* a sensor without a gyro: one made from gravity; up and down only */
	{
		struct input_motion_info mi;
		float y0;

		imu_my = 0;
		input_motion_sensor(&(struct input_motion_desc){ "ACC-1", "ACC", MOTION_CH_ACCEL, 0, 0.002f });
		/* a gyro's zero in the calibration is not this sensor's to take */
		input_set_motion_calibration(&(struct motion_cal){ .axes_set = true, .right = { 1, 0, 0 },
								   .bias_set = true, .bias = { 1, 1, 1 } });
		CHECK(input_motion()->bias[0] == 0);
		input_motion_info(&mi);
		CHECK(mi.bias_known && !mi.aim_x && mi.aim_y);
		imu_feed(300, 0, 0, 0, 0, 0);
		y0 = imu_my;
		imu_turned = 0;
		imu_feed(500, 0, 0, 0, 0, 20);	/* the top 20 degrees towards the player */
		imu_feed(200, 0, 0, 0, 20, 20);
		CHECK(fabsf(imu_my - y0 - 20) < 1.5f && fabsf(imu_turned) < 0.5f);
		imu_turned = 0;
		imu_feed(500, 0, 0, 90, 20, 20);	/* a gyro's turn is not seen */
		CHECK(fabsf(imu_turned) < 0.5f);
		input_set_motion_calibration(NULL);
	}

	/* no gyro, plugged out and in again (or the stream lost for a while)
	 * while the device was turned: no jump of the view when it comes back */
	{
		imu_my = 0;
		input_motion_sensor(&(struct input_motion_desc){ "ACC-2", "ACC", MOTION_CH_ACCEL, 0, 0.002f });
		input_set_motion_calibration(&(struct motion_cal){ .axes_set = true, .right = { 1, 0, 0 } });
		input_motion_present(true, 1000);
		imu_feed(2000, 0, 0, 0, 0, 0);
		input_motion_present(false, 0);
		input_motion_present(true, 1000);
		imu_my = 0;
		imu_feed(2000, 0, 0, 0, 60, 60);
		CHECK(fabsf(imu_my) < 1);
		imu_feed(500, 0, 0, 0, 0, 0);	/* the gap in the stream */
		input_motion_gap();
		imu_my = 0;
		imu_feed(2000, 0, 0, 0, 60, 60);
		CHECK(fabsf(imu_my) < 1);
		input_motion_present(false, 0);
		input_set_motion_calibration(NULL);
	}

	/* neither: nothing to aim with, the samples are not used */
	{
		struct input_motion_info mi;

		input_motion_sensor(&(struct input_motion_desc){ "MAG-1", "MAG", 0, 0, 0 });
		input_motion_info(&mi);
		CHECK(!mi.aim_x && !mi.aim_y && !mi.channels);
		imu_turned = 0;
		imu_feed(1500, 0, 0, 60, 0, 0);
		CHECK(imu_turned == 0);
	}

	/* devices plugged in: the listener hears each change once */
	input_set_devices_cb(on_imu_devices);
	input_report_devices(INPUT_DEV_KEYBOARD | INPUT_DEV_MOUSE, INPUT_DEV_MOUSE);
	input_motion_present(true, 1000);
	input_motion_present(true, 1000);
	CHECK(imu_devices_calls == 2 && imu_devices_seen == (INPUT_DEV_MOUSE | INPUT_DEV_MOTION));
	CHECK(input_devices() == (INPUT_DEV_MOUSE | INPUT_DEV_MOTION));
	input_motion_present(false, 0);
	input_report_devices(INPUT_DEV_KEYBOARD | INPUT_DEV_MOUSE, 0);
	CHECK(imu_devices_calls == 4 && imu_devices_seen == 0);
	input_set_devices_cb(NULL);
	input_deinit();
	mainloop_free(ml);
}
#endif

struct rtp_capture {
	uint8_t pkt[16][RTP_HEADER + RTP_MAX_PAYLOAD];
	size_t len[16];
	int n;
};

static void rtp_capture(void *ud, const uint8_t *p, size_t len)
{
	struct rtp_capture *c = ud;

	if (c->n < 16) {
		memcpy(c->pkt[c->n], p, len);
		c->len[c->n++] = len;
	}
}

static void test_rtp(void)
{
	static uint8_t au[4000];
	static struct rtp_capture c;
	struct rtp_track t = { .ssrc = 0x11223344, .seq = 65535, .pt = 96 };
	int16_t pcm[4] = { 1, -2, 256, -32768 };
	uint8_t sr[64];
	size_t n, i;

	/* SPS (5 bytes), PPS (4), an IDR slice of 3000: three FU-A packets */
	n = 0;
	memcpy(au + n, "\0\0\0\1\x67\x42\xe0\x1f\x01", 9);
	n += 9;
	memcpy(au + n, "\0\0\0\1\x68\xce\x38\x80", 8);
	n += 8;
	memcpy(au + n, "\0\0\1\x65", 4);
	n += 4;
	for (i = 0; i < 2999; i++)
		au[n++] = (uint8_t)(i % 251 + 1);
	rtp_h264(&t, au, n, 90000, rtp_capture, &c);
	CHECK(c.n == 5);
	CHECK(c.len[0] == RTP_HEADER + 5 && c.pkt[0][RTP_HEADER] == 0x67 && !(c.pkt[0][1] & 0x80));
	CHECK(c.len[1] == RTP_HEADER + 4 && c.pkt[1][RTP_HEADER] == 0x68);
	CHECK(c.pkt[0][2] == 0xff && c.pkt[0][3] == 0xff && c.pkt[1][2] == 0 && c.pkt[1][3] == 0);
	/* FU-A: indicator 0x7c (NRI of 0x65, type 28), S then E, marker on the last */
	CHECK(c.pkt[2][RTP_HEADER] == 0x7c && c.pkt[2][RTP_HEADER + 1] == 0x85);
	CHECK(c.pkt[3][RTP_HEADER + 1] == 0x05 && !(c.pkt[3][1] & 0x80));
	CHECK(c.pkt[4][RTP_HEADER + 1] == 0x45 && (c.pkt[4][1] & 0x80));
	CHECK(c.len[2] - RTP_HEADER - 2 + c.len[3] - RTP_HEADER - 2 + c.len[4] - RTP_HEADER - 2 == 2999);
	CHECK(c.pkt[2][RTP_HEADER + 2] == 1 && c.pkt[4][c.len[4] - 1] == (uint8_t)(2998 % 251 + 1));
	CHECK(c.pkt[0][4] == 0 && c.pkt[0][5] == 1 && c.pkt[0][6] == 0x5f && c.pkt[0][7] == 0x90);
	CHECK(t.packets == 5);
	/* L16: big-endian */
	c.n = 0;
	t.pt = 97;
	rtp_l16(&t, pcm, 2, 480, rtp_capture, &c);
	CHECK(c.n == 1 && c.len[0] == RTP_HEADER + 8);
	CHECK(!memcmp(c.pkt[0] + RTP_HEADER, "\0\1\xff\xfe\x01\0\x80\0", 8));
	/* SR + SDES: whole words, the length fields right */
	n = rtcp_sr(&t, 1000000ULL * 1700000000, 1234, "bare_app", sr, sizeof(sr));
	CHECK(n % 4 == 0 && n == 28 + 4 * (size_t)(sr[31] + 1));
	CHECK(sr[1] == 200 && sr[3] == 6 && sr[29] == 202 && sr[37] == 8);
	CHECK(sr[8] == 0xe8 && sr[9] == 0xfe && sr[10] == 0x6f && sr[11] == 0x80);	/* NTP seconds */
}

/* What players send back: RR (+ SDES), SR with a block, BYE, PLI, junk */
static void test_rtcp_in(void)
{
	/* RR from 0x1234 about 0xcafe0001: 25% lost (64/256), -1 in all,
	 * highest 0x10005, jitter 900, LSR 0x00010000, DLSR 0x8000 (0.5 s) */
	static const uint8_t rr_sdes[] = {
		0x81, 201, 0, 7, 0, 0, 0x12, 0x34,
		0xca, 0xfe, 0, 1, 64, 0xff, 0xff, 0xff, 0, 1, 0, 5, 0, 0, 3, 0x84,
		0, 1, 0, 0, 0, 0, 0x80, 0,
		0x81, 202, 0, 2, 0, 0, 0x12, 0x34, 1, 2, 'p', 'c',
	};
	static const uint8_t sr_block[] = {
		0x81, 200, 0, 12, 0, 0, 0x12, 0x34,
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
		0xca, 0xfe, 0, 2, 3, 0, 0, 7, 0, 0, 0, 9, 0, 0, 0, 90, 0, 0, 0, 0, 0, 0, 0, 0,
	};
	static const uint8_t bye_pli[] = {
		0x81, 206, 0, 2, 0, 0, 0x12, 0x34, 0xca, 0xfe, 0, 1,
		0x81, 203, 0, 1, 0, 0, 0x12, 0x34,
	};
	static const uint8_t cut[] = { 0x81, 201, 0, 7, 0, 0, 0x12, 0x34, 0xca, 0xfe };
	static const uint8_t rtp[] = { 0x80, 96, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1 };
	struct rtcp_info in;
	struct rtcp_block b = { .dlsr = 0x8000 };	/* 0.5 s at the player */
	uint64_t at;

	CHECK(rtcp_parse(rr_sdes, sizeof(rr_sdes), &in));
	CHECK(in.blocks == 1 && !in.bye && !in.key_frame);
	CHECK(in.block[0].ssrc == 0xcafe0001 && in.block[0].fraction == 64 && in.block[0].lost == -1);
	CHECK(in.block[0].highest == 0x10005 && in.block[0].jitter == 900);
	CHECK(in.block[0].lsr == 0x10000 && in.block[0].dlsr == 0x8000);
	CHECK(rtcp_parse(sr_block, sizeof(sr_block), &in));
	CHECK(in.blocks == 1 && in.block[0].ssrc == 0xcafe0002 && in.block[0].fraction == 3 &&
	      in.block[0].lost == 7 && in.block[0].jitter == 90 && !in.block[0].lsr);
	CHECK(rtcp_parse(bye_pli, sizeof(bye_pli), &in));
	CHECK(in.blocks == 0 && in.bye && in.key_frame);
	CHECK(!rtcp_parse(cut, sizeof(cut), &in));	/* says 32 bytes, has 10 */
	CHECK(!rtcp_parse(rtp, sizeof(rtp), &in));
	/* the round trip: now - LSR - DLSR */
	at = 1000000ULL * 1700000000;
	b.lsr = rtcp_ntp_mid(at) - 0x18000;	/* our SR left 1.5 s ago */
	CHECK(rtcp_rtt_ms(&b, at) == 1000);
	b.lsr = 0;
	CHECK(rtcp_rtt_ms(&b, at) == -1);
}

struct ts_capture {
	uint8_t data[64 * TS_PACKET];
	size_t len;
	int groups;
};

static void ts_capture(void *ud, const uint8_t *p, size_t len)
{
	struct ts_capture *c = ud;

	CHECK(len % TS_PACKET == 0 && len <= TS_GROUP * TS_PACKET);
	if (c->len + len <= sizeof(c->data)) {
		memcpy(c->data + c->len, p, len);
		c->len += len;
	}
	c->groups++;
}

/* The payload of pid's packets from at, put together; their count */
static int ts_payload(const struct ts_capture *c, int pid, uint8_t *out, size_t *len, bool *pcr)
{
	size_t i;
	int n = 0, cc = -1;

	*len = 0;
	*pcr = false;
	for (i = 0; i < c->len; i += TS_PACKET) {
		const uint8_t *p = c->data + i;
		size_t off = 4;

		CHECK(p[0] == 0x47);
		if (((p[1] & 0x1f) << 8 | p[2]) != pid)
			continue;
		if (cc >= 0)
			CHECK((p[3] & 15) == ((cc + 1) & 15));
		cc = p[3] & 15;
		if (p[3] & 0x20) {
			if (n == 0 && p[4] >= 7 && (p[5] & 0x10))
				*pcr = true;
			off += 1 + p[4];
		}
		memcpy(out + *len, p + off, TS_PACKET - off);
		*len += TS_PACKET - off;
		n++;
	}
	return n;
}

static void test_ts(void)
{
	static struct ts_capture c;
	static uint8_t au[1000], pay[16 * TS_PACKET];
	struct ts_mux m;
	int16_t pcm[480 * 2];
	size_t len, i;
	bool pcr;
	uint32_t crc = 0xffffffff;
	int b;

	ts_init(&m, true);
	memcpy(au, "\0\0\0\1\x65", 5);	/* an IDR slice without SPS: the params go first */
	for (i = 5; i < sizeof(au); i++)
		au[i] = (uint8_t)i;
	ts_video(&m, au, sizeof(au), true, (const uint8_t *)"\0\0\0\1\x67\x42", 6, 99000, 90000,
		 ts_capture, &c);
	/* PAT, PMT, then the PES: 14 + 6 (AUD) + 6 + 1000 bytes in 6 packets */
	CHECK(c.len == 8 * TS_PACKET && c.groups == 2);
	CHECK(c.data[1] == 0x40 && c.data[2] == 0 && c.data[5] == 0);	/* PAT */
	for (i = 5; i < 5 + 16; i++)	/* its CRC checks out: 0 over the whole section */
		for (crc ^= (uint32_t)c.data[i] << 24, b = 0; b < 8; b++)
			crc = crc & 0x80000000 ? crc << 1 ^ 0x04c11db7 : crc << 1;
	CHECK(crc == 0);
	CHECK(ts_payload(&c, TS_PID_VIDEO, pay, &len, &pcr) == 6 && !pcr);	/* PCR on the sound */
	CHECK(len == 14 + 6 + 6 + 1000);
	CHECK(!memcmp(pay, "\0\0\1\xe0", 4) && pay[7] == 0x80 && pay[8] == 5);
	CHECK(pay[9] == 0x21 && pay[13] == (uint8_t)(99000 << 1 | 1));	/* PTS 99000 */
	CHECK(!memcmp(pay + 14, "\0\0\0\1\x09\xf0\0\0\0\1\x67\x42\0\0\0\1\x65", 17));
	CHECK(!memcmp(pay + 26 + 5, au + 5, sizeof(au) - 5));
	/* 10 ms of sound: one PES, 18 + 1920 bytes in 11 packets, the PCR in the first */
	c.len = 0;
	for (i = 0; i < 960; i++)
		pcm[i] = (int16_t)(i * 7);
	ts_audio(&m, pcm, 480, 99900, 90900, ts_capture, &c);
	CHECK(ts_payload(&c, TS_PID_AUDIO, pay, &len, &pcr) == 11 && pcr && len == 18 + 1920);
	CHECK(pay[3] == 0xbd && (pay[4] << 8 | pay[5]) == 3 + 5 + 4 + 1920);
	CHECK((pay[14] << 8 | pay[15]) == 1920 && pay[16] == 0x31 && pay[17] == 0x40);
	CHECK(pay[18 + 2 * 959] == (uint8_t)(959 * 7 >> 8) && pay[18 + 2 * 959 + 1] == (uint8_t)(959 * 7));
}

static void test_msgq(void)
{
	struct msgq *q = msgq_new(sizeof(int), 3);
	int v, i;

	for (i = 1; i <= 3; i++)
		CHECK(msgq_post(q, &i));
	i = 4;
	CHECK(!msgq_post(q, &i));		/* full */
	for (i = 1; i <= 3; i++) {
		CHECK(msgq_pop(q, &v));
		CHECK(v == i);			/* FIFO */
	}
	CHECK(!msgq_pop(q, &v));
	msgq_free(q);
}

static void test_ctx(void)
{
	struct gui_ctx snap;
	struct gui_ctx *c;

	CHECK(ctx_init() == 0);
	CHECK(ctx_snapshot(&snap) == CTX_ALL);	/* first snapshot: everything */
	CHECK(ctx_snapshot(&snap) == 0);

	/* two writes before the view looks: one change, latest value */
	c = ctx_lock();
	c->power.capacity = 50;
	ctx_unlock(CTX_POWER);
	c = ctx_lock();
	c->power.capacity = 49;
	c->display.brightness = 70;
	ctx_unlock(CTX_POWER | CTX_DISPLAY);
	CHECK(ctx_snapshot(&snap) == (CTX_POWER | CTX_DISPLAY));
	CHECK(snap.power.capacity == 49);
	CHECK(snap.display.brightness == 70);
	ctx_deinit();
}

static void test_nav(void)
{
	struct control_env env = { 0 };
	struct gui_action a;
	struct gui_ctx snap;

	CHECK(ctx_init() == 0);
	ctl_nav.init(&env);
	ctx_snapshot(&snap);
	CHECK(ctx_screen(&snap) == SCREEN_HOME);

	a = (struct gui_action){ .type = ACT_BACK, .arg = 0 };
	ctl_nav.on_action(&a);			/* home has nothing to go back to */
	CHECK(ctx_snapshot(&snap) == 0);

	a = (struct gui_action){ .type = ACT_OPEN_SCREEN, .arg = SCREEN_SETTINGS };
	ctl_nav.on_action(&a);
	ctl_nav.on_action(&a);			/* same screen twice: one entry */
	CHECK(ctx_snapshot(&snap) == CTX_NAV);
	CHECK(ctx_screen(&snap) == SCREEN_SETTINGS && snap.nav.depth == 2);

	a = (struct gui_action){ .type = ACT_OPEN_SCREEN, .arg = SCREEN_COUNT };
	ctl_nav.on_action(&a);			/* invalid id ignored */
	CHECK(ctx_snapshot(&snap) == 0);

	a = (struct gui_action){ .type = ACT_BACK, .arg = 0 };
	ctl_nav.on_action(&a);
	CHECK(ctx_snapshot(&snap) == CTX_NAV);
	CHECK(ctx_screen(&snap) == SCREEN_HOME && snap.nav.depth == 1);
	ctx_deinit();
}

static void test_uevent(void)
{
	static const char msg[] =
		"add@/devices/platform/adc-joystick/input/input5/event5\0"
		"ACTION=add\0DEVPATH=/devices/platform/adc-joystick/input/input5/event5\0"
		"SUBSYSTEM=input\0MAJOR=13\0MINOR=69\0DEVNAME=input/event5\0SEQNUM=1234";
	static const char udev[] = "libudev\0\xfe\xed\xca\xfe";
	char buf[sizeof(msg)];
	struct uevent ev;

	memcpy(buf, msg, sizeof(msg));
	CHECK(uevent_parse(buf, sizeof(buf), &ev) == 0);
	CHECK(!strcmp(ev.action, "add") && !strcmp(ev.subsystem, "input"));
	CHECK(ev.devname && !strcmp(ev.devname, "input/event5"));
	CHECK(ev.name == NULL);

	memcpy(buf, udev, sizeof(udev));
	CHECK(uevent_parse(buf, sizeof(udev), &ev) < 0);	/* udev monitor format ignored */
}

static void test_config(void)
{
	const char *path = "test_config.conf";
	struct mainloop *ml = mainloop_new("test");
	FILE *f;

	unlink(path);
	config_open(ml, path);
	CHECK(config_get("lang") == NULL);
	CHECK(config_get_int("brightness", -1) == -1);
	config_set("lang", "ru");
	config_set_int("brightness", 55);
	config_close();				/* writes the delayed save now */

	f = fopen(path, "r");
	CHECK(f != NULL);
	if (f)
		fclose(f);
	config_open(ml, path);
	CHECK(config_get("lang") && !strcmp(config_get("lang"), "ru"));
	CHECK(config_get_int("brightness", -1) == 55);
	config_close();
	unlink(path);

	config_open(ml, NULL);			/* memory only */
	config_set("lang", "en");
	CHECK(!strcmp(config_get("lang"), "en"));
	config_close();
	mainloop_free(ml);
}

/* Producer fills every pixel of a frame with its number; a torn frame
 * would mix two numbers, a lost order would go backwards. */
#define MB_FRAMES 3000

static void *mb_producer(void *arg)
{
	struct mailbox *mb = arg;
	uint32_t n;
	int i;

	for (n = 1; n <= MB_FRAMES; n++) {
		struct hal_fb *fb = mailbox_begin(mb);

		for (i = 0; i < fb->width * fb->height; i++)
			fb->pixels[i] = n;
		mailbox_end(mb);
	}
	return NULL;
}

static void test_mailbox(void)
{
	struct mailbox *mb = mailbox_new(64, 32);
	uint32_t last = 0, taken = 0;
	pthread_t t;
	int i, torn = 0, backwards = 0;

	CHECK(mb && mailbox_take(mb) == NULL);
	pthread_create(&t, NULL, mb_producer, mb);
	while (last < MB_FRAMES) {
		struct hal_fb *fb = mailbox_take(mb);

		if (!fb)
			continue;
		for (i = 1; i < fb->width * fb->height; i++)
			torn += fb->pixels[i] != fb->pixels[0];
		backwards += fb->pixels[0] <= last;
		last = fb->pixels[0];
		taken++;
	}
	pthread_join(t, NULL);
	CHECK(torn == 0);
	CHECK(backwards == 0);
	CHECK(taken + mailbox_dropped(mb) == MB_FRAMES);
	mailbox_free(mb);
}

/* ---- input: repeat and stick -> D-pad, driven by a real loop ---------- */

static struct {
	int down, up, repeats;
	enum gp_button last;
} seen;

static void on_input(const struct gp_event *ev, void *ud)
{
	(void)ud;
	if (ev->type != GP_EV_BUTTON)
		return;
	seen.last = ev->code;
	if (ev->repeat)
		seen.repeats++;
	else if (ev->pressed)
		seen.down++;
	else
		seen.up++;
}

static void release_right(void *ud)
{
	input_report_button(GP_RIGHT, false);
	mainloop_quit(ud);
}

static void test_input(void)
{
	struct input_config cfg = { .repeat_delay_ms = 50, .repeat_period_ms = 20,
				    .stick_press = 0.5f, .stick_release = 0.3f };
	struct mainloop *ml = mainloop_new("test");

	input_init(ml, &cfg, on_input, NULL);

	/* stick: past press -> UP, inside hysteresis -> still UP, back -> released */
	input_report_axis(GP_AXIS_LY, -0.7f);
	CHECK(seen.down == 1 && seen.last == GP_UP && (input_buttons() & GP_BIT(GP_UP)));
	input_report_axis(GP_AXIS_LY, -0.4f);
	CHECK(seen.up == 0);
	input_report_axis(GP_AXIS_LY, 0);
	CHECK(seen.up == 1 && !(input_buttons() & GP_BIT(GP_UP)));

	/* D-pad held stays pressed even when the stick lets go of the same direction */
	input_report_button(GP_UP, true);
	input_report_axis(GP_AXIS_LY, -0.9f);
	input_report_axis(GP_AXIS_LY, 0);
	CHECK(input_buttons() & GP_BIT(GP_UP));
	input_report_button(GP_UP, false);

	/* held direction repeats: 50 ms delay, then every 20 ms; hold for 130 ms */
	memset(&seen, 0, sizeof(seen));
	input_report_button(GP_RIGHT, true);
	mainloop_add_timer(ml, 130, 0, release_right, ml);
	mainloop_run(ml);
	CHECK(seen.down == 1 && seen.up == 1);
	CHECK(seen.repeats >= 3 && seen.repeats <= 5);

	/* a stick's dead zone and sensitivity, kept in range, from and to the file */
	{
		const char *path = "test_sticks.conf";
		struct input_stick_tune t = { .deadzone = 0.2f, .sensitivity = 2 };
		FILE *f;

		input_set_stick_tune(1, &t);
		input_report_axis(GP_AXIS_RX, 0.1f);	/* in the dead zone */
		CHECK(NEAR(input_axis(GP_AXIS_RX), 0));
		input_report_axis(GP_AXIS_RX, 0.4f);	/* (0.4 - 0.2) / 0.8 * 2 */
		CHECK(NEAR(input_axis(GP_AXIS_RX), 0.5f));
		input_report_axis(GP_AXIS_RX, 0.9f);	/* cut at the edge */
		CHECK(NEAR(input_axis(GP_AXIS_RX), 1));
		t.deadzone = 3, t.sensitivity = 0;
		input_set_stick_tune(1, &t);
		input_stick_tune(1, &t);
		CHECK(NEAR(t.deadzone, INPUT_DEADZONE_MAX) && NEAR(t.sensitivity, INPUT_SENS_MIN));

		f = fopen(path, "w");
		CHECK(f != NULL);
		if (f) {
			fputs("left.deadzone = 0.07\nleft.sensitivity = 1.25\nright.bogus = 1\n", f);
			fclose(f);
		}
		CHECK(input_load_calibration(path) == 0);
		input_stick_tune(0, &t);
		CHECK(NEAR(t.deadzone, 0.07f) && NEAR(t.sensitivity, 1.25f));
		input_set_stick_tune(0, NULL);	/* the default: cfg's dead zone */
		input_stick_tune(0, &t);
		CHECK(NEAR(t.deadzone, cfg.stick_deadzone) && NEAR(t.sensitivity, 1));
		t.deadzone = 0.3f;
		input_set_stick_tune(0, &t);
		CHECK(input_save_calibration() == 0);
		input_set_stick_tune(0, NULL);
		CHECK(input_load_calibration(path) == 0);
		input_stick_tune(0, &t);
		CHECK(NEAR(t.deadzone, 0.3f));
		unlink(path);
		input_report_axis(GP_AXIS_RX, 0);
	}

	input_deinit();
	mainloop_free(ml);
}

static struct {
	int keys, key_code, key_pressed, buttons, button, button_pressed, moves, mbuttons;
} hid_seen;
static bool hid_raw;
static bool hid_mouse_only;	/* a game that takes the mouse but not the keys (Doom) */

static void on_hid_input(const struct gp_event *ev, void *ud)
{
	(void)ud;
	if (ev->type == GP_EV_KEY) {
		hid_seen.keys++;
		hid_seen.key_code = ev->code;
		hid_seen.key_pressed = ev->pressed;
	} else if (ev->type == GP_EV_BUTTON && !ev->repeat) {
		hid_seen.buttons++;
		hid_seen.button = ev->code;
		hid_seen.button_pressed = ev->pressed;
	} else if (ev->type == GP_EV_MOUSE_MOVE) {
		hid_seen.moves++;
	} else if (ev->type == GP_EV_MOUSE_BUTTON) {
		hid_seen.mbuttons++;
	}
}

static bool hid_raw_query(bool mouse)
{
	return hid_raw || (mouse && hid_mouse_only);
}

/* Keyboards and mice: gamepad buttons in the UI, as they are to a game */
static void test_hid(void)
{
	struct mainloop *ml = mainloop_new("test");

	input_init(ml, NULL, on_hid_input, NULL);
	input_set_raw_query(hid_raw_query);
	memset(&hid_seen, 0, sizeof(hid_seen));
	hid_raw = false;
	input_report_key(28 /* KEY_ENTER */, true);
	CHECK(hid_seen.buttons == 1 && hid_seen.button == GP_A && hid_seen.button_pressed);
	hid_raw = true;		/* a game comes to the front: the release still frees A */
	input_report_key(28, false);
	CHECK(hid_seen.buttons == 2 && hid_seen.button == GP_A && !hid_seen.button_pressed);
	CHECK(hid_seen.keys == 0);
	input_report_key(30 /* KEY_A */, true);
	CHECK(hid_seen.keys == 1 && hid_seen.key_code == 30 && hid_seen.key_pressed);
	hid_raw = false;	/* back to the menu: the game still gets the release */
	input_report_key(30, false);
	CHECK(hid_seen.keys == 2 && !hid_seen.key_pressed);
	input_report_key(30, true);	/* a letter means nothing to the menu */
	input_report_key(30, false);
	CHECK(hid_seen.keys == 2 && hid_seen.buttons == 2);
	input_report_mouse_move(5, 0, 0);
	input_report_mouse_button(0, true);
	CHECK(hid_seen.moves == 0 && hid_seen.mbuttons == 0);
	hid_raw = true;
	input_report_mouse_move(5, -3, 1);
	input_report_mouse_button(0, true);
	hid_raw = false;
	input_report_mouse_button(0, false);
	CHECK(hid_seen.moves == 1 && hid_seen.mbuttons == 2);
	hid_mouse_only = true;	/* the mouse as it is, keys as buttons */
	input_report_mouse_move(1, 1, 0);
	input_report_key(28, true);
	input_report_key(28, false);
	CHECK(hid_seen.moves == 2 && hid_seen.keys == 2 && hid_seen.buttons == 4 && hid_seen.button == GP_A);
	hid_mouse_only = false;
	CHECK(input_key_button(1 /* KEY_ESC */) == GP_B && input_key_button(30) == GP_NONE);
	input_deinit();
	mainloop_free(ml);
}

static void test_volume(void)
{
	/* rk817 "Master Playback Volume": -95..0 dB in 0.01 dB */
	int p, db, back;

	CHECK(volume_db_to_percent(-9500, -9500, 0) == 0);
	CHECK(volume_db_to_percent(0, -9500, 0) == 100);
	CHECK(volume_percent_to_db(0, -9500, 0) == -9500);
	CHECK(volume_percent_to_db(100, -9500, 0) == 0);
	/* half way sounds like half: about -18 dB, nowhere near -47.5 */
	db = volume_percent_to_db(50, -9500, 0);
	CHECK(db > -2000 && db < -1600);
	for (p = 0; p <= 100; p++) {
		db = volume_percent_to_db(p, -9500, 0);
		back = volume_db_to_percent(db, -9500, 0);
		CHECK(back == p);
		if (p > 0)
			CHECK(db > volume_percent_to_db(p - 1, -9500, 0));
	}
	CHECK(volume_db_to_percent(-500, 0, 0) == 100);	/* no range */
}

#if defined(CONFIG_WIFI) && defined(CONFIG_PLATFORM_OGA)
static void test_wpa(void)
{
	static const char results[] =
		"bssid / frequency / signal level / flags / ssid\n"
		"10:7c:61:80:07:40\t2432\t-48\t[WPA2-PSK-CCMP][WPS][ESS]\t46B\n"
		"10:7c:61:80:07:41\t2412\t-80\t[WPA2-PSK-CCMP][ESS]\t46B\n"
		"aa:bb:cc:00:00:01\t2437\t-70\t[ESS]\tCafe \\\"Free\\\"\n"
		"aa:bb:cc:00:00:02\t2462\t-90\t[RSN-SAE-CCMP][ESS]\t\\xd0\\x94\\xd0\\xbe\\xd0\\xbc\n"
		"aa:bb:cc:00:00:03\t2462\t-60\t[WPA2-PSK-CCMP][ESS]\t\\x00\\x00\n"
		"aa:bb:cc:00:00:04\t2462\t-30\t[WEP][ESS]\t\n";
	static const char saved[] =
		"network id / ssid / bssid / flags\n"
		"0\t46B\tany\t[CURRENT]\n"
		"1\tsobs1992\tany\t\n";
	struct hal_wifi_net nets[8];
	char out[40];
	int n;

	wpa_unescape("a\\\\b\\\"c\\x41\\n", out, sizeof(out));
	CHECK(!strcmp(out, "a\\b\"cA\n"));
	wpa_unescape("abcdef", out, 4);
	CHECK(!strcmp(out, "abc"));

	n = wpa_parse_scan(results, saved, nets, 8);
	CHECK(n == 3);		/* 46B once, hidden ones dropped */
	CHECK(!strcmp(nets[0].ssid, "46B") && nets[0].signal == 100 && nets[0].secured &&
	      nets[0].saved);
	CHECK(!strcmp(nets[1].ssid, "Cafe \"Free\"") && nets[1].signal == 60 && !nets[1].secured &&
	      !nets[1].saved);
	CHECK(!strcmp(nets[2].ssid, "\xd0\x94\xd0\xbe\xd0\xbc") && nets[2].signal == 20 &&
	      nets[2].secured);
	CHECK(wpa_parse_scan(results, NULL, nets, 1) == 1);
}
#endif

/* blit_rotated() against a pixel by pixel turn, as fbcon=rotate:N */
static void test_blit(void)
{
	enum { LW = 37, LH = 21, PAD = 3 };	/* odd sizes: tiles end mid-way */
	static const struct hal_rect rects[] = {
		{ 0, 0, LW - 1, LH - 1 }, { 5, 3, 30, 19 }, { 0, 0, 0, 0 },
		{ LW - 1, LH - 1, LW - 1, LH - 1 }, { 2, 0, 2, LH - 1 }, { 0, 7, LW - 1, 7 },
	};
	static uint32_t src_px[LH][LW + PAD];
	static uint8_t out[LW * (LH + PAD) * 4], want[sizeof(out)];
	struct hal_fb src = { &src_px[0][0], LW, LH, (LW + PAD) * 4, 0 };
	int x, y, rot, mode, i;

	for (y = 0; y < LH; y++)
		for (x = 0; x < LW + PAD; x++)
			src_px[y][x] = (uint32_t)(x * 2654435761u ^ y * 40503u) | (x % 3 ? 0xff000000 : 0);
	for (rot = 0; rot < 4; rot++) {
		for (mode = BLIT_COPY; mode <= BLIT_RGB565; mode++) {
			int bpp = mode == BLIT_RGB565 ? 2 : 4;
			int nw = rot & 1 ? LH : LW, nh = rot & 1 ? LW : LH;
			struct blit_dst dst = { out, nw, nh, (nw + PAD) * bpp };

			for (i = 0; i < (int)(sizeof(rects) / sizeof(rects[0])); i++) {
				const struct hal_rect *r = &rects[i];
				struct blit_dst ref = { want, nw, nh, dst.stride };

				memset(out, 0x5a, sizeof(out));
				memset(want, 0x5a, sizeof(want));
				for (y = r->y1; y <= r->y2; y++) {
					for (x = r->x1; x <= r->x2; x++) {
						struct hal_fb one = { &src_px[y][x], 1, 1, 4, 0 };
						int nx = rot == 0 ? x : rot == 1 ? nw - 1 - y : rot == 2 ? nw - 1 - x : y;
						int ny = rot == 0 ? y : rot == 1 ? x : rot == 2 ? nh - 1 - y : nh - 1 - x;

						/* the conversion of one pixel, placed by hand */
						blit_rotated(&(struct blit_dst){ want + ny * ref.stride + nx * bpp, 1, 1, bpp },
							     &one, &(struct hal_rect){ 0, 0, 0, 0 }, 0, mode);
					}
				}
				blit_rotated(&dst, &src, r, rot, mode);
				CHECK(memcmp(out, want, sizeof(out)) == 0);
			}
		}
	}

	/* blit_compose: opaque wins, transparent shows the bottom, half red over blue */
	{
		uint32_t t[3] = { 0xff123456, 0x00ffffff, 0x80ff0000 };
		uint32_t b[3] = { 0xff0000ff, 0x000000ff, 0xff0000ff };
		uint32_t d[3] = { 0 };
		struct hal_fb top = { t, 3, 1, sizeof(t), 0 }, bottom = { b, 3, 1, sizeof(b), 0 };
		struct hal_fb out = { d, 3, 1, sizeof(d), 0 };

		blit_compose(&out, &top, &bottom, &(struct hal_rect){ 0, 0, 2, 0 });
		CHECK(d[0] == 0xff123456 && d[1] == 0xff0000ff && d[2] == 0xff80007f);
	}
}

static void test_damage(void)
{
	struct damage d = { 0 };
	struct hal_rect all = { 0, 0, 99, 49 }, part = { 10, 10, 20, 20 }, other = { 30, 5, 40, 8 };

	damage_add(&d, &part, 100, 50);
	damage_add(&d, &other, 100, 50);
	CHECK(d.n == 2);
	damage_add(&d, &part, 100, 50);		/* covered: nothing new */
	CHECK(d.n == 2);
	damage_add(&d, &all, 100, 50);		/* covers both */
	CHECK(d.n == 1 && memcmp(&d.r[0], &all, sizeof(all)) == 0);
	damage_add(&d, &all, 100, 50);		/* the next frame, the same area */
	CHECK(d.n == 1);
	damage_add(&d, &(struct hal_rect){ -5, -5, 200, 200 }, 100, 50);	/* clipped to the same */
	CHECK(d.n == 1);
}

#ifdef CONFIG_SYSMON
/* Write root/rel, making the directories on the way */
static void put(const char *root, const char *rel, const char *text)
{
	char path[512], *p;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	for (p = path + strlen(root) + 1; (p = strchr(p, '/')); p++) {
		*p = '\0';
		mkdir(path, 0755);
		*p = '/';
	}
	f = fopen(path, "w");
	if (!f) {
		CHECK(!"cannot write the fake tree");
		return;
	}
	fputs(text, f);
	fclose(f);
}

static void put_procs(const char *root, int self, int game, unsigned long game_ticks)
{
	char rel[64], text[256];

	snprintf(rel, sizeof(rel), "proc/%d/stat", self);
	snprintf(text, sizeof(text), "%d (bare_app) S 1 1 1 0 -1 4194560 100 0 0 0 10 5 0 0 20 0 3 0 "
		 "100 1000000 2560 18446744073709551615\n", self);
	put(root, rel, text);
	snprintf(rel, sizeof(rel), "proc/%d/stat", game);
	/* a name with a space and a bracket, as /proc gives it */
	snprintf(text, sizeof(text), "%d (vcmi (main) x) R %d %d %d 0 -1 0 0 0 0 0 %lu 0 0 0 20 0 9 0 "
		 "200 900000000 25600\n", game, self, game, game, game_ticks);
	put(root, rel, text);
	put(root, "proc/1/stat", "1 (init) S 0 1 1 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 1 100 50\n");
}

/* A GO Ultra-like system in a directory: two clusters, a battery zone to
 * leave out, an eMMC with boot areas, a game started by bare_app */
static void test_sysmon(void)
{
	char root[] = "/tmp/bare_app_sysmon_XXXXXX";
	struct sysmon_info in;
	struct sysmon *s;
	int self = (int)getpid(), game = self + 1, i;
	char cmd[64];

	if (!mkdtemp(root)) {
		CHECK(!"mkdtemp");
		return;
	}
	put(root, "proc/cpuinfo", "processor\t: 0\nCPU part\t: 0xd03\n\nprocessor\t: 1\n"
				  "CPU part\t: 0xd09\n\nprocessor\t: 2\nCPU part\t: 0xd09\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy0/related_cpus", "0\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", "1000000\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq", "1704000\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy1/related_cpus", "1 2\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy1/scaling_cur_freq", "1398000\n");
	put(root, "sys/devices/system/cpu/cpufreq/policy1/cpuinfo_max_freq", "1908000\n");
	put(root, "proc/stat", "cpu  150 0 150 1050 150 0 0 0\n"
			       "cpu0 50 0 50 350 50 0 0 0\ncpu1 50 0 50 350 50 0 0 0\n"
			       "cpu2 50 0 50 350 50 0 0 0\nintr 1 2 3\n");
	put(root, "proc/meminfo", "MemTotal:        2048000 kB\nMemFree:          100000 kB\n"
				  "MemAvailable:    1024000 kB\nSwapTotal:             0 kB\n"
				  "SwapFree:              0 kB\n");
	put(root, "sys/class/thermal/thermal_zone0/type", "cpu-thermal\n");
	put(root, "sys/class/thermal/thermal_zone0/temp", "45500\n");
	put(root, "sys/class/thermal/thermal_zone1/type", "rk818-battery\n");
	put(root, "sys/class/thermal/thermal_zone1/temp", "20000\n");
	put(root, "sys/class/thermal/thermal_zone10/type", "ddr-thermal\n");
	put(root, "sys/class/thermal/thermal_zone10/temp", "40000\n");
	put(root, "sys/class/devfreq/ffe40000.gpu/cur_freq", "400000000\n");
	put(root, "sys/class/devfreq/ffe40000.gpu/max_freq", "800000000\n");
	put(root, "sys/class/power_supply/rk818-usb/type", "USB\n");
	put(root, "sys/class/power_supply/rk818-battery/type", "Battery\n");
	put(root, "sys/class/power_supply/rk818-battery/capacity", "87\n");
	put(root, "sys/class/power_supply/rk818-battery/voltage_now", "3900000\n");
	put(root, "sys/class/power_supply/rk818-battery/current_now", "500000\n");	/* wrong sign */
	put(root, "sys/class/power_supply/rk818-battery/status", "Discharging\n");
	put(root, "sys/block/mmcblk1/device/type", "MMC\n");
	put(root, "sys/block/mmcblk1boot0/device/type", "MMC\n");
	put(root, "sys/block/loop0/size", "0\n");
	put(root, "proc/diskstats", " 179 0 mmcblk1 10 0 2000 0 5 0 1000 0 0 100 0\n"
				    " 179 3 mmcblk1p3 1 0 8 0 0 0 0 0 0 0 0\n");
	put(root, "proc/mounts", "/dev/mmcblk1p3 /data ext4 rw 0 0\n"
				 "/dev/mmcblk1p1 /boot vfat rw 0 0\ntmpfs /tmp tmpfs rw 0 0\n");
	put(root, "data/.keep", "");
	put(root, "proc/uptime", "3725.50 100.00\n");
	put_procs(root, self, game, 100);

	s = sysmon_open(root);
	CHECK(s != NULL);
	if (!s)
		return;
	sysmon_sample(s, 1000, &in);
	CHECK(in.ncpus == 3 && in.cpu_total == -1 && in.cpu[0] == -1);	/* no loads yet */
	CHECK(in.nclusters == 2);
	CHECK(!strcmp(in.clusters[0].core, "A53") && in.clusters[0].first == 0 &&
	      in.clusters[0].count == 1 && in.clusters[0].mhz == 1000 && in.clusters[0].max_mhz == 1704);
	CHECK(!strcmp(in.clusters[1].core, "A73") && in.clusters[1].first == 1 &&
	      in.clusters[1].count == 2 && in.clusters[1].mhz == 1398);
	CHECK(in.mem_total_mb == 2000 && in.mem_used_mb == 1000);
	CHECK(in.ntemps == 2);				/* the battery's zone left out */
	CHECK(in.temps[0].kind == SYSMON_TEMP_CPU && !strcmp(in.temps[0].name, "cpu") &&
	      in.temps[0].mdeg == 45500);
	CHECK(in.temps[1].kind == SYSMON_TEMP_DDR && in.temps[1].mdeg == 40000);
	CHECK(in.gpu_mhz == 400 && in.gpu_max_mhz == 800);
	CHECK(in.battery && in.bat_capacity == 87 && in.bat_mv == 3900);
	CHECK(in.bat_ma == -500 && in.bat_mw == -1950);	/* out of the battery */
	CHECK(in.ndisks == 1);				/* no boot areas, no loop */
	CHECK(!strcmp(in.disks[0].name, "mmcblk1") && in.disks[0].kind == SYSMON_DISK_EMMC);
	CHECK(in.disks[0].busy == -1);
	CHECK(in.disks[0].nmounts == 1 && !strcmp(in.disks[0].mounts[0].path, "/data"));
	CHECK(in.disks[0].mounts[0].total_mb > 0 &&
	      in.disks[0].mounts[0].used_mb <= in.disks[0].mounts[0].total_mb &&
	      in.disks[0].used_mb == in.disks[0].mounts[0].used_mb &&
	      in.disks[0].free_mb == in.disks[0].mounts[0].free_mb);
	CHECK(in.nprocs == 2 && in.procs[0].self && in.procs[0].pid == self);
	CHECK(!strcmp(in.procs[0].name, "bare_app") && in.procs[0].cpu == -1);
	CHECK(in.procs[0].rss_mb == (uint32_t)(2560 * (sysconf(_SC_PAGESIZE) / 1024) / 1024));
	CHECK(!in.procs[1].self && in.procs[1].pid == game && !strcmp(in.procs[1].name, "vcmi (main) x"));
	CHECK(in.uptime_s == 3725);

	/* a second later: cpu0 half busy, cpu1 and cpu2 all busy; the disk read
	 * 1 MB, wrote 2 MB, busy a quarter of the time; the game used a CPU */
	put(root, "proc/stat", "cpu  400 0 150 1100 150 0 0 0\n"
			       "cpu0 100 0 50 400 50 0 0 0\ncpu1 150 0 50 350 50 0 0 0\n"
			       "cpu2 150 0 50 350 50 0 0 0\n");
	put(root, "proc/diskstats", " 179 0 mmcblk1 20 0 4000 0 9 0 5000 0 0 350 0\n");
	put_procs(root, self, game, 100 + sysconf(_SC_CLK_TCK));
	sysmon_sample(s, 2000, &in);
	CHECK(in.cpu[0] == 50 && in.cpu[1] == 100 && in.cpu[2] == 100);
	CHECK(in.cpu_total == 83);			/* 250 of 300 */
	CHECK(in.disks[0].read_kbs == 1024 && in.disks[0].write_kbs == 2048);
	CHECK(in.disks[0].busy == 25);
	CHECK(in.procs[0].cpu == 0 && in.procs[1].cpu == 100);
	sysmon_close(s);

	snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
	i = system(cmd);
	(void)i;
}
#endif

int main(void)
{
	test_joystick();
	test_msgq();
	test_ctx();
	test_nav();
	test_uevent();
	test_config();
	test_mailbox();
	test_input();
	test_volume();
	test_blit();
	test_damage();
	test_frametimes();
	test_remap();
	test_motion();
#ifdef CONFIG_INPUT_IMU
	test_imu();
#endif
	test_rtp();
	test_rtcp_in();
	test_ts();
	test_hid();
#ifdef CONFIG_SYSMON
	test_sysmon();
#endif
#if defined(CONFIG_WIFI) && defined(CONFIG_PLATFORM_OGA)
	test_wpa();
#endif
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("all core tests passed");
	return 0;
}
