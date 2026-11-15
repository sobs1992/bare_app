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
		float est = motion_synth_setup(&sy, 0.0022f, 1125);
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
		CHECK(fabsf(sy.span - 0.0996f) < 0.001f && est > 0.2f && est < 0.35f);
		CHECK(sqrt(sq / n) < 0.5 && peak < 2);
		CHECK(fabsf(deg - 25) < 2);
		CHECK(motion_rec_right(&r, axis) && axis[0] > 0.99f);
		/* the same accelerometer at 104 Hz: each sample counts more, the
		 * longest span, a bigger estimate; unknown noise: the longest */
		est = motion_synth_setup(&sy, 0.0022f, 104);
		CHECK(sy.span == MOTION_SYNTH_MAX_S && est > 0.5f && est < 0.9f);
		CHECK(motion_synth_setup(&sy, 0, 0) == 0 && sy.span == MOTION_SYNTH_MAX_S);
		/* the span is not stretched for a slow sensor: its window holds one
		 * sample then (no "gap" at every step: tested through the parser) */
		motion_synth_setup(&sy, 0.0001f, 10);
		CHECK(fabsf(sy.span - 0.0001f * sqrtf(1139.0f / 10) * 45) < 1e-3f && sy.span < 0.1f);
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

/* An accelerometer-only stream at hz: n samples (one a frame) tilting about
 * x from tilt0 to tilt1 degrees; *sq and *t go on */
static size_t imu_acc_samples(uint8_t *out, uint16_t *sq, uint32_t *t, float hz, int n, float tilt0,
			      float tilt1)
{
	size_t m = 0;
	uint8_t hdr[8 + 12];

	for (int i = 0; i < n; i++) {
		float th = (tilt0 + (tilt1 - tilt0) * (i + 1) / n) * 0.017453293f;
		int16_t ay = (int16_t)lrintf(sinf(th) * 8192), az = (int16_t)lrintf(cosf(th) * 8192);

		memcpy(hdr, t, 4);
		memcpy(hdr + 4, sq, 2);
		hdr[6] = 1;
		hdr[7] = 0;
		memset(hdr + 8, 0, 12);
		memcpy(hdr + 8 + 8, &ay, 2);
		memcpy(hdr + 8 + 10, &az, 2);
		m += imu_frame(out + m, 0x04, hdr, 8 + 12);
		(*sq)++;
		*t += (uint32_t)lrintf(1e6f / hz);
	}
	return m;
}

/* The sensor (accelerometer only, at hz) told by its info, then calibrated:
 * its right is the sensor's x */
static void imu_acc_start(struct imu_parser *p, uint8_t *buf, const char *serial, float hz)
{
	uint8_t st3[36] = { 3, 0, 3, 0x0b };
	size_t m;

	imu_parser_init(p);
	snprintf(p->serial, sizeof(p->serial), "%s", serial);
	m = imu_info(buf, "ACCONLY", 9, 2, (uint32_t)lrintf(hz * 1000), 0, 1.0f / 8192, 0, 0.002f);
	m += imu_frame(buf + m, 0x02, st3, 36);
	imu_parser_feed(p, buf, m);
	input_set_motion_calibration(&(struct motion_cal){ .axes_set = true, .right = { 1, 0, 0 } });
}

/*
 * The motion sensor's scenario matrix: modes (gyro and accelerometer, gyro
 * alone, accelerometer alone) x rates (1, 5, 104, 1000 Hz) x events in the
 * stream, all through the parser (imu_parser_feed()). A simulated device
 * tilts about the sensor's x (its right) and turns about z (its vertical,
 * lying flat); the samples say its rates and its gravity, a gap loses
 * samples (their numbers skipped) or time (numbers going on). What the view
 * must do: take the turns the received samples show, never a jump for what
 * happened in a gap; with a gyro the tilt is the gyro's (a gap's tilt is
 * never seen), an accelerometer alone sees a tilt over a gap of 3 samples or
 * less (its gravity shows it) but takes no longer gap's.
 */
enum { MM_GA, MM_G, MM_A, MM_MODES };

static const uint8_t mm_channels[MM_MODES] = { 1 | 2, 1, 2 };

struct mstream {
	uint8_t *buf;
	size_t len, cap;
	uint16_t seq;
	uint32_t t;
	float hz, tilt;		/* the device's rate (may differ from the info's), tilt about x */
	int batch;		/* samples a frame, as the firmware: as many as come in 4 ms, 1..4 */
	int pending;		/* samples in the frame being made */
	int last_n;		/* ...in the last frame that went out */
	uint8_t frame[8 + 12 * 4];
};

static void ms_put(struct mstream *s, const uint8_t *frame, size_t n)
{
	if (s->len + n > s->cap) {
		s->cap = (s->len + n) * 2;
		s->buf = realloc(s->buf, s->cap);
	}
	memcpy(s->buf + s->len, frame, n);
	s->len += n;
}

/* the frame being made goes out (its time and number: its first sample's) */
static void ms_flush(struct mstream *s)
{
	uint8_t f[8 + 12 * 4 + 6];

	if (!s->pending)
		return;
	s->frame[6] = (uint8_t)s->pending;
	s->frame[7] = 0;
	ms_put(s, f, imu_frame(f, 0x04, s->frame, (uint8_t)(8 + 12 * s->pending)));
	s->last_n = s->pending;
	s->pending = 0;
}

/* n samples turning wx about x and wz about z (deg/s) */
static void ms_run(struct mstream *s, int n, float wx, float wz)
{
	for (int i = 0; i < n; i++) {
		int16_t g[3] = { (int16_t)lrintf(wx * 10), 0, (int16_t)lrintf(wz * 10) };	/* 0.1 deg/s */
		int16_t a[3];

		s->tilt += wx / s->hz;
		a[0] = 0;
		a[1] = (int16_t)lrintf(sinf(s->tilt * 0.017453293f) * 8192);
		a[2] = (int16_t)lrintf(cosf(s->tilt * 0.017453293f) * 8192);
		if (!s->pending) {
			memcpy(s->frame, &s->t, 4);
			memcpy(s->frame + 4, &s->seq, 2);
		}
		memcpy(s->frame + 8 + 12 * s->pending, g, 6);
		memcpy(s->frame + 8 + 12 * s->pending + 6, a, 6);
		if (++s->pending >= s->batch)
			ms_flush(s);
		s->seq++;
		s->t += (uint32_t)lrintf(1e6f / s->hz);
	}
}

/* n samples turning w (deg/s) about the world's vertical, however the device
 * is tilted (about x): its gyro sees it about (0, sin tilt, cos tilt) */
static void ms_run_vert(struct mstream *s, int n, float w)
{
	float th = s->tilt * 0.017453293f;

	for (int i = 0; i < n; i++) {
		int16_t g[3] = { 0, (int16_t)lrintf(sinf(th) * w * 10), (int16_t)lrintf(cosf(th) * w * 10) };
		int16_t a[3] = { 0, (int16_t)lrintf(sinf(th) * 8192), (int16_t)lrintf(cosf(th) * 8192) };

		if (!s->pending) {
			memcpy(s->frame, &s->t, 4);
			memcpy(s->frame + 4, &s->seq, 2);
		}
		memcpy(s->frame + 8 + 12 * s->pending, g, 6);
		memcpy(s->frame + 8 + 12 * s->pending + 6, a, 6);
		if (++s->pending >= s->batch)
			ms_flush(s);
		s->seq++;
		s->t += (uint32_t)lrintf(1e6f / s->hz);
	}
}

/* a gap: 'missing' samples lost, 'extra' seconds with none, the device
 * tilted by 'tilt' meanwhile (and turned too: no sample shows it) */
static void ms_gap(struct mstream *s, int missing, float extra, float tilt)
{
	ms_flush(s);
	s->seq = (uint16_t)(s->seq + missing);
	s->t += (uint32_t)lrintf(missing * 1e6f / s->hz + extra * 1e6f);
	s->tilt += tilt;
}

static void ms_feed(struct imu_parser *p, struct mstream *s)
{
	ms_flush(s);
	imu_parser_feed(p, s->buf, s->len);
	s->len = 0;
}

static int ms_n(float hz, float secs, int at_least)
{
	int n = (int)lrintf(hz * secs);

	return n < at_least ? at_least : n;
}

static size_t mm_info(uint8_t *f, int mode, float hz)
{
	uint8_t ch = mm_channels[mode];

	/* (both scales, as the firmware sends them: the channels say which count) */
	return imu_info(f, "MATRIX", 9, ch, (uint32_t)lrintf(hz * 1000), 0.1f, 1.0f / 8192,
			ch & 1 ? 0.1f : 0, ch & 2 ? 0.002f : 0);
}

/* A fresh sensor of the mode, hz in its info, real_hz its real rate,
 * calibrated (right x; vertical z where there is no gravity), lain still
 * long enough for its zero (and its rate measured); its stream in s */
static void mm_start3(struct imu_parser *p, struct mstream *s, int mode, float hz, float real_hz,
		      unsigned id, float still);
static bool mm_no_bias;	/* the next start's calibration has no gyro zero */

static void mm_start2(struct imu_parser *p, struct mstream *s, int mode, float hz, float real_hz,
		      unsigned id)
{
	mm_start3(p, s, mode, hz, real_hz, id, 0);
}

/* ...lain still for 'still' seconds (0: 2 s and 10 batches, with the rate
 * off the info's 3 s and 18 batches: measured many times over); its gyro's
 * zero known from the calibration (0) */
static void mm_start3(struct imu_parser *p, struct mstream *s, int mode, float hz, float real_hz,
		      unsigned id, float still)
{
	uint8_t f[160], st3[36] = { 3, 0, 3, 0x0b };	/* info 66 + status 42 bytes */
	size_t n;
	int b = (int)(4000 / (1e6f / hz));	/* as the firmware: from the info's rate */

	input_set_motion_calibration(NULL);	/* the last one's: room for this one's */
	imu_parser_init(p);
	snprintf(p->serial, sizeof(p->serial), "MM%u", id);
	n = mm_info(f, mode, hz);
	n += imu_frame(f + n, 0x02, st3, 36);
	imu_parser_feed(p, f, n);
	input_set_motion_calibration(&(struct motion_cal){ .axes_set = true, .right = { 1, 0, 0 },
							   .up_set = !(mm_channels[mode] & 2),
							   .up = { 0, 0, 1 }, .bias_set = !mm_no_bias });
	s->len = 0;
	s->pending = 0;
	s->hz = real_hz;
	s->batch = b < 1 ? 1 : b > 4 ? 4 : b;
	s->tilt = 0;
	s->seq = 1000;
	s->t = 0;
	/* (measured each 0.25 s and 8 intervals; before the first measure a
	 * pause counts only beyond twice the info's period) */
	if (still > 0)
		ms_run(s, ms_n(real_hz, still, 1), 0, 0);
	else
		ms_run(s, real_hz == hz ? ms_n(real_hz, 2, 10 * s->batch)
					: ms_n(real_hz, 3, 18 * s->batch), 0, 0);
	ms_feed(p, s);
	imu_turned = imu_my = 0;
}

static void mm_start(struct imu_parser *p, struct mstream *s, int mode, float hz, unsigned id)
{
	mm_start2(p, s, mode, hz, hz, id);
}

static void mm_check2(const char *what, int mode, float hz, float x, float y, float xtol, float ytol)
{
	static const char *const names[MM_MODES] = { "gyro+accel", "gyro", "accel" };

	if (fabsf(imu_turned - x) <= xtol && fabsf(imu_my - y) <= ytol)
		return;
	fprintf(stderr, "matrix %s, %s, %g Hz: x %.2f y %.2f, want %.1f %.1f\n", what, names[mode],
		(double)hz, (double)imu_turned, (double)imu_my, (double)x, (double)y);
	failures++;
}

static void mm_check(const char *what, int mode, float hz, float x, float y, float tol)
{
	mm_check2(what, mode, hz, x, y, tol, tol);
}

static void test_motion_matrix(void)
{
	static const float rates[] = { 1, 5, 104, 1000 };
	static const int gaps[] = { 4, 50, 900 };
	struct imu_parser p;
	struct mainloop *ml = mainloop_new("matrix");
	struct mstream s = { 0 };
	unsigned id = 0;

	input_init(ml, NULL, on_imu_event, NULL);
	for (int mode = 0; mode < MM_MODES; mode++) {
		bool gyro = mode != MM_A;

		for (unsigned ri = 0; ri < sizeof(rates) / sizeof(rates[0]); ri++) {
			float hz = rates[ri];
			int nturn = ms_n(hz, 0.1f, 4);		/* 20 degrees in these */
			float w = 20 * hz / nturn;

			/* a steady stream: a turn of 45 degrees left, a tilt of 20;
			 * read in pieces, "present" said after each (the driver says
			 * it every second): nothing of it changes */
			{
				int n1 = ms_n(hz, 1, 4);

				mm_start(&p, &s, mode, hz, id++);
				for (int h = 0; h < 2; h++) {
					ms_run(&s, n1 / 2 + (h ? n1 % 2 : 0), 0, 45 * hz / n1);
					ms_feed(&p, &s);
					input_motion_present(true, hz);
				}
				ms_run(&s, ms_n(hz, 2, 3), 0, 0);
				for (int h = 0; h < 2; h++) {
					ms_run(&s, n1 / 2 + (h ? n1 % 2 : 0), 20 * hz / n1, 0);
					ms_feed(&p, &s);
					input_motion_present(true, hz);
				}
				ms_run(&s, ms_n(hz, 3, 4), 0, 0);
				ms_feed(&p, &s);
				input_motion_present(false, 0);
				mm_check("steady", mode, hz, gyro ? -45 : 0, 20, hz >= 104 ? 0.6f : 2.5f);
			}

			/* the real rate other than the info says (0.5x to 2x, the
			 * ends too): measured from the samples' times, the turn as it
			 * was; then 3 lost + 0.4 period paused is no gap, 3 + 0.6 is,
			 * in the real rate's periods */
			{
				static const float off[] = { 0.5f, 0.8f, 1.18f, 1.5f, 2.0f };

				for (unsigned oi = 0; oi < 5; oi++) {
					float rh = hz * off[oi];
					int nt = ms_n(rh, 1, 4);

					mm_start2(&p, &s, mode, hz, rh, id++);
					ms_run(&s, nt, 0, 45 * rh / nt);
					ms_run(&s, nt, 20 * rh / nt, 0);
					ms_run(&s, ms_n(rh, 3, 4), 0, 0);
					ms_feed(&p, &s);
					mm_check("real rate off the info's", mode, rh, gyro ? -45 : 0, 20, 2.5f);
					for (int ci = 0; ci < 2; ci++) {
						imu_turned = imu_my = 0;
						ms_gap(&s, 3, (ci ? 0.6f : 0.4f) / rh, 20);
						ms_run(&s, ms_n(rh, 3, 4), 0, 0);
						ms_feed(&p, &s);
						mm_check("real rate off, then lost and paused", mode, rh, 0,
							 ci == 0 && !gyro ? 20 : 0, 2);
					}
					/* ...and a gap (50 lost) keeps the measure: a turn
					 * right after it as it was (about the world's
					 * vertical, tilted by 60 by now: a gyro alone cos 60
					 * of it) */
					imu_turned = imu_my = 0;
					ms_gap(&s, 50, 0, 0);
					ms_run_vert(&s, ms_n(rh, 0.2f, 4), 20 * rh / ms_n(rh, 0.2f, 4));
					ms_run(&s, ms_n(rh, 2, 4), 0, 0);
					ms_feed(&p, &s);
					mm_check("real rate off, a gap, a turn at once", mode, rh,
						 mode == MM_GA ? -20 : mode == MM_G ? -10 : 0, 0,
						 hz >= 104 ? 0.6f : 2.5f);
				}
			}

			if (hz >= 104) {
				/* 0.5..2x is taken (RATE_SLOWEST 2.02: 2.015x is, 2.025x
				 * is not) */
				static const float off[] = { 0.5f, 1.5f, 2.0f, 1 / 2.015f, 2.015f };
				static const float outside[] = { 0.3f, 0.45f, 1 / 2.025f, 2.025f, 2.2f, 3.0f };
				int nt;

				/* plugged in with the rate off the info's, a turn 0.3 s
				 * later: measured each 0.25 s */
				for (unsigned oi = 0; oi < 5; oi++) {
					float rh = hz * off[oi];
					int nt = ms_n(rh, 1, 4);

					mm_start3(&p, &s, mode, hz, rh, id++, 0.3f);
					ms_run(&s, nt, 0, 45 * rh / nt);
					ms_run(&s, nt, 20 * rh / nt, 0);
					ms_run(&s, ms_n(rh, 2, 4), 0, 0);
					ms_feed(&p, &s);
					mm_check("rate off, a turn at once", mode, rh, gyro ? -45 : 0, 20, 0.6f);
				}
				/* a rate beyond 0.5..2x of the info's (just beyond too) is
				 * not taken: the info's period counts (a gyro's turn by
				 * its ratio); then a long pause (30 of the info's
				 * periods) with a tilt in it is a gap all the same */
				for (unsigned oi = 0; oi < 6; oi++) {
					float rh = hz * outside[oi], k = outside[oi];
					int nt = ms_n(rh, 1, 4);

					mm_start2(&p, &s, mode, hz, rh, id++);
					ms_run(&s, nt, 0, 45 * rh / nt);
					ms_run(&s, nt, 20 * rh / nt, 0);
					ms_run(&s, ms_n(rh, 2, 4), 0, 0);
					ms_feed(&p, &s);
					mm_check2("rate beyond 0.5..2x: the info's", mode, rh, gyro ? -45 * k : 0,
						  gyro ? 20 * k : 20, 0.6f * fmaxf(1, k), 0.6f * fmaxf(1, k));
					imu_turned = imu_my = 0;
					ms_gap(&s, 0, 30 / hz, 20);
					ms_run(&s, ms_n(rh, 2, 4), 0, 0);
					ms_feed(&p, &s);
					mm_check("rate beyond, a long pause", mode, rh, 0, 0, 1);
				}
				/* a long pause (30 of the info's periods: beyond anything
				 * a rate within 2x could make) right after the plug,
				 * before the first measure, with a tilt in it: a gap */
				mm_start3(&p, &s, mode, hz, hz, id++, 0.1f);
				ms_gap(&s, 0, 30 / hz, 20);
				ms_run(&s, ms_n(hz, 2, 4), 0, 0);
				ms_feed(&p, &s);
				mm_check("long pause before the first measure", mode, hz, 0, 0, 1);
				/* measured (1.5x), then beyond 2x without a new info
				 * (4x): the info's period again, not the last measure */
				mm_start2(&p, &s, mode, hz, 1.5f * hz, id++);
				s.hz = 4 * hz;
				ms_run(&s, ms_n(s.hz, 0.6f, 8), 0, 0);
				ms_feed(&p, &s);
				imu_turned = imu_my = 0;
				nt = ms_n(s.hz, 1, 4);
				ms_run(&s, nt, 0, 45 * s.hz / nt);
				ms_run(&s, ms_n(s.hz, 1, 4), 0, 0);
				ms_feed(&p, &s);
				mm_check("measured, then beyond 2x: the info's", mode, s.hz, gyro ? -45 * 4 : 0, 0,
					 2.4f);
				/* the rate halves or doubles (or changes a little: 1.18x,
				 * 1.03x and 0.97x, beyond RATE_SAME's 2 %) without a new
				 * info (the firmware never does it): taken
				 * within two windows of the measure (0.25 s each: the one
				 * it falls in counts its majority), a turn 0.55 s later as
				 * it was, wherever in a window it falls (4 places) */
				for (int ki = 0; ki < 5; ki++)
					for (int ph = 0; ph < 4; ph++) {
						static const float k[5] = { 0.5f, 2.0f, 1.18f, 1.03f, 0.97f };
						char what[64];

						mm_start(&p, &s, mode, hz, id++);
						ms_run(&s, ph * ms_n(hz, 0.25f / 4, 1), 0, 0);
						s.hz = hz * k[ki];
						ms_run(&s, ms_n(s.hz, 0.55f, 8), 0, 0);
						ms_feed(&p, &s);
						imu_turned = imu_my = 0;
						nt = ms_n(s.hz, 1, 4);
						ms_run(&s, nt, 0, 45 * s.hz / nt);
						ms_run(&s, nt, 20 * s.hz / nt, 0);
						ms_run(&s, ms_n(s.hz, 2, 4), 0, 0);
						ms_feed(&p, &s);
						snprintf(what, sizeof(what), "rate %gx, no info, phase %d", (double)k[ki], ph);
						mm_check(what, mode, s.hz, gyro ? -45 : 0, 20, 0.6f);
					}
				/* a new info mid-stream: twice the rate (it does run so),
				 * a turn right after (about the world's vertical: with
				 * gravity 45, a gyro alone cos 60 of it, see below) */
				{
					uint8_t f[80];
					int nt = ms_n(2 * hz, 1, 4);

					mm_start(&p, &s, mode, hz, id++);
					/* tilted between the last sample of the old rate
					 * and the first of the new, numbers and times going
					 * on: the first sample of a new rate is a gap (the
					 * "up" of before is stale) */
					ms_gap(&s, 0, 0, 60);
					ms_put(&s, f, mm_info(f, mode, 2 * hz));
					s.hz = 2 * hz;
					s.batch = (int)fminf(4, fmaxf(1, 2 * hz * 0.004f));
					ms_run_vert(&s, nt, 45 * 2 * hz / nt);
					ms_run(&s, ms_n(2 * hz, 2, 4), 0, 0);
					ms_feed(&p, &s);
					mm_check("new info, twice the rate", mode, 2 * hz,
						 mode == MM_GA ? -45 : mode == MM_G ? -22.5f : 0, 0, 0.6f);
				}
				/* a new info (twice the rate) while the real rate stays:
				 * no measure for it yet, nothing told, and no batch is a
				 * gap (an accelerometer alone sees a tilt right after it;
				 * a gyro's counts by the new info's period till measured) */
				{
					uint8_t f[80];
					struct imu_rate_told told = { 0 };
					int nt2 = ms_n(hz, 0.1f, 4);

					mm_start(&p, &s, mode, hz, id++);
					imu_rate_news(&p, &told);
					ms_flush(&s);
					ms_put(&s, f, mm_info(f, mode, 2 * hz));
					s.batch = (int)fminf(4, fmaxf(1, 2 * hz * 0.004f));
					ms_run(&s, 2 * s.batch, 0, 0);	/* (its first sample: a gap) */
					ms_run(&s, nt2, 20 * hz / nt2, 0);
					ms_feed(&p, &s);
					CHECK(imu_rate_news(&p, &told) == 0 && !p.period_measured);
					ms_run(&s, ms_n(hz, 3, 4), 0, 0);
					ms_feed(&p, &s);
					mm_check("new info, the real rate stays", mode, hz, 0, gyro ? 10 : 20, 1);
				}
				/* a new info 1.95 % off the old rate (the real one with
				 * it): measured anew, not from the old rate's windows */
				{
					uint8_t f[80];
					float nh = hz * 1.0195f;

					mm_start(&p, &s, mode, hz, id++);
					ms_flush(&s);
					ms_put(&s, f, mm_info(f, mode, nh));
					s.hz = nh;
					ms_run(&s, ms_n(nh, 0.3f, 8), 0, 0);
					ms_feed(&p, &s);
					CHECK(p.period_measured && fabsf(p.period_us - 1e6f / nh) < 0.005f * 1e6f / nh);
				}
				/* a new info with the same rate (another sensor: the fake
				 * one's modes): measured anew too, nothing told before */
				{
					uint8_t f[80];
					struct imu_rate_told told = { 0 };
					int other = mode == MM_GA ? MM_G : MM_GA;

					mm_start(&p, &s, mode, hz, id++);
					imu_rate_news(&p, &told);
					ms_flush(&s);
					ms_put(&s, f, mm_info(f, other, hz));
					ms_run(&s, 2 * s.batch, 0, 0);
					ms_feed(&p, &s);
					CHECK(imu_rate_news(&p, &told) == 0 && !p.period_measured);
					ms_run(&s, ms_n(hz, 0.3f, 8), 0, 0);
					ms_feed(&p, &s);
					CHECK(imu_rate_news(&p, &told) == IMU_RATE_MEASURED && p.period_measured);
				}
				/* at the limit (0.5x and 2x of the info's: 1 % from the
				 * measure's edge, more than a window's error), a pause
				 * in the measure's window, wherever it falls in it, in the
				 * first window too: samples lost (3 + 0.4 period, 3 + 0.6:
				 * a gap, 1 + 0.3) or time only (0.15, 0.2, 0.45, 0.55
				 * period; 1 and 2: data-ready missed), then a turn (two
				 * batches later; after 0.3 s in the first window): the
				 * measure stays taken, the turn as it was */
				{
					static const float edge[2] = { 0.5f, 2.0f };
					static const int ev_lost[] = { 3, 3, 1, 0, 0, 0, 0, 0, 0 };
					static const float ev_pause[] = { 0.4f, 0.6f, 0.3f, 0.15f, 0.2f, 0.45f, 0.55f, 1, 2 };

					for (int fw = 0; fw < 2; fw++)
						for (int ei = 0; ei < 2; ei++)
							for (int vi = 0; vi < 9; vi++)
								for (int ph = 0; ph < 8; ph++) {
									float rh = hz * edge[ei];
									int n2 = ms_n(rh, 0.2f, 4);
									char what[80];

									if (fw)
										mm_start3(&p, &s, mode, hz, rh, id++, 0.01f);
									else
										mm_start2(&p, &s, mode, hz, rh, id++);
									ms_run(&s, ph * ms_n(rh, 0.25f / 8, 1), 0, 0);
									ms_gap(&s, ev_lost[vi], ev_pause[vi] / rh, 0);
									ms_run(&s, fw ? ms_n(rh, 0.3f, 8) : 2 * s.batch, 0, 0);
									ms_feed(&p, &s);
									imu_turned = imu_my = 0;
									ms_run(&s, n2, 0, 20 * rh / n2);
									ms_run(&s, ms_n(rh, 1, 4), 0, 0);
									ms_feed(&p, &s);
									snprintf(what, sizeof(what), "at the edge%s, %d lost + %.2g, phase %d",
										 fw ? " (first window)" : "", ev_lost[vi],
										 (double)ev_pause[vi], ph);
									mm_check(what, mode, rh, gyro ? -20 : 0, 0, 0.6f);
								}
				}
				/* the rate halves without an info a few samples into the
				 * first window, or a later one: a window where nothing
				 * counts against the mean of its middle half makes no
				 * measure (no 0/0), the next does */
				for (int at = 5; at <= 13 + 9; at++) {
					if (at <= 13) {
						mm_start3(&p, &s, mode, hz, hz, id++, 0.001f);
						ms_run(&s, at, 0, 0);
					} else {
						mm_start(&p, &s, mode, hz, id++);
						ms_run(&s, at - 9, 0, 0);
					}
					s.hz = hz / 2;
					for (int c = 0; c < 10; c++) {
						ms_run(&s, ms_n(s.hz, 0.1f, 4), 0, 0);
						ms_feed(&p, &s);
						CHECK(isfinite(p.period_us) && isfinite(p.period_refused) &&
						      !(p.period_refused > 0));
					}
					CHECK(p.period_measured && fabsf(p.period_us - 1e6f / s.hz) < 0.002f * 1e6f / s.hz);
				}
				/* a little change of the rate without an info (1.5 %:
				 * within RATE_SAME): the windows take it over 8 of them,
				 * all of it after 2.3 s */
				mm_start(&p, &s, mode, hz, id++);
				s.hz = hz * 1.015f;
				ms_run(&s, ms_n(s.hz, 2.3f, 8), 0, 0);
				ms_feed(&p, &s);
				CHECK(fabsf(p.period_us - 1e6f / s.hz) < 0.001f * 1e6f / s.hz);
				/* the same info again (asked for again, a scale for the
				 * channel it lacks in it, as the firmware sends it): no new
				 * stream, no gap (an accelerometer alone sees the tilt
				 * across it), the measure kept */
				{
					uint8_t f[80];
					unsigned seq_was;
					int nt2 = ms_n(hz, 0.1f, 4);

					mm_start(&p, &s, mode, hz, id++);
					seq_was = p.info_seq;
					ms_run(&s, nt2 / 2, 20 * hz / nt2, 0);
					ms_flush(&s);
					ms_put(&s, f, mm_info(f, mode, hz));
					ms_run(&s, nt2 - nt2 / 2, 20 * hz / nt2, 0);
					ms_run(&s, ms_n(hz, 3, 4), 0, 0);
					ms_feed(&p, &s);
					CHECK(p.info_seq == seq_was && p.period_measured);
					mm_check("the same info again", mode, hz, 0, 20, 0.6f);
				}
			}

			/* "present" said again (every second) is no gap: gravity's
			 * "up" kept, a knock in the first sample after it (70 degrees
			 * off in one sample: beyond what the yaw's relaxing hides) is
			 * not taken for it; a turn about the vertical right then as
			 * it was */
			if (mode == MM_GA && hz >= 104) {
				int nt = ms_n(hz, 1, 4);

				mm_start(&p, &s, mode, hz, id++);
				input_motion_present(true, hz);
				ms_gap(&s, 0, 0, 70);
				ms_run(&s, 1, 0, 0);
				ms_gap(&s, 0, 0, -70);
				ms_run_vert(&s, nt, 45 * hz / nt);
				ms_run(&s, ms_n(hz, 2, 4), 0, 0);
				ms_feed(&p, &s);
				input_motion_present(false, 0);
				mm_check("present again, a jolt, a turn", mode, hz, -45, 0, 0.6f);
			}
			/* a gap keeps the measure: right after it a pause of 3.6
			 * measured periods is a gap again (an accelerometer alone
			 * does not take the tilt in it) */
			mm_start(&p, &s, mode, hz, id++);
			ms_gap(&s, 50, 0, 0);
			ms_run(&s, ms_n(hz, 0.05f, 2), 0, 0);
			ms_gap(&s, 0, 3.6f / hz, 20);
			ms_run(&s, ms_n(hz, 3, 4), 0, 0);
			ms_feed(&p, &s);
			mm_check("a gap, then 3.6 periods paused", mode, hz, 0, 0, 1);
			/* a frame twice (sent again): its samples once */
			{
				int nt = ms_n(hz, 1, 4);
				uint8_t again[8 + 12 * 4 + 6];
				size_t fl;
				uint32_t had;

				mm_start(&p, &s, mode, hz, id++);
				had = p.samples;
				ms_run(&s, nt / 2, 0, 45 * hz / nt);
				ms_flush(&s);
				fl = 8 + 12 * (size_t)s.last_n + 6;
				memcpy(again, s.buf + s.len - fl, fl);
				ms_put(&s, again, fl);
				ms_run(&s, nt - nt / 2, 0, 45 * hz / nt);
				ms_run(&s, ms_n(hz, 2, 4), 0, 0);
				ms_feed(&p, &s);
				mm_check("a frame twice", mode, hz, gyro ? -45 : 0, 0, hz >= 104 ? 0.6f : 2.5f);
				CHECK(p.samples - had == (uint32_t)(nt + ms_n(hz, 2, 4)));
			}

			/* before the first measure the info's period may be off by
			 * up to 2x (RATE_SLOWEST): a pause counts only beyond twice
			 * the expected time, in such periods; 3.4 of them is no gap (an
			 * accelerometer alone sees the tilt in it), 3.6 is */
			for (int ci = 0; ci < 2; ci++) {
				float many = ci ? 3.6f : 3.4f;
				int last;

				mm_start3(&p, &s, mode, hz, hz, id++, 0.02f);
				last = s.last_n;	/* the interval's samples: the last frame's */
				ms_gap(&s, 0, (2.02f * many + 1.02f * last) / hz, 20);
				ms_run(&s, ms_n(hz, 3, 4), 0, 0);
				ms_feed(&p, &s);
				mm_check(ci ? "3.6 slow periods paused before the measure"
					    : "3.4 slow periods paused before the measure", mode, hz, 0,
					 ci == 0 && !gyro ? 20 : 0, 2);
			}
			/* ...so at half the info's rate no batch is a gap before it:
			 * an accelerometer alone sees a tilt then (a gyro's turn counts
			 * by the info's period until the measure) */
			if (!gyro) {
				int nt = ms_n(hz / 2, 0.1f, 4);

				mm_start3(&p, &s, mode, hz, hz / 2, id++, 0.02f);
				ms_run(&s, nt, 20 * hz / 2 / nt, 0);
				ms_run(&s, ms_n(hz / 2, 3, 4), 0, 0);
				ms_feed(&p, &s);
				mm_check("half the rate, a tilt before the measure", mode, hz / 2, 0, 20, 2);
			}
			/* turned 45 and tilted 20, a gap right after, all in one read:
			 * what the samples showed is the view's whole. Without a gyro
			 * the part of the tilt the made-up gyro has not given yet is
			 * lost in a gap (not told from a jolt): 0.3 s after the tilt
			 * 15 % at most, 0.5 s after 3 %; right at the end of a tilt
			 * over 1 s 14..18 % (at 104 Hz and more) */
			for (int si = 0; si < (gyro ? 1 : 3); si++) {
				int nt = ms_n(hz, 1, 4);

				if (si == 2 && hz < 104)
					continue;
				mm_start(&p, &s, mode, hz, id++);
				ms_run(&s, nt, 0, 45 * hz / nt);
				ms_run(&s, nt, 20 * hz / nt, 0);
				if (!gyro && si < 2)
					ms_run(&s, ms_n(hz, si ? 0.3f : 0.5f, 2), 0, 0);
				ms_gap(&s, 50, 0, 0);
				ms_run(&s, ms_n(hz, 2, 4), 0, 0);
				ms_feed(&p, &s);
				if (si == 2)
					mm_check2("tilted over 1 s, then a gap at once", mode, hz, 0, 16.8f, 0.6f, 0.4f);
				else if (si)
					mm_check2("tilted, 0.3 s, then a gap", mode, hz, 0, 18.8f, 0.6f, 1.8f);
				else
					mm_check("turned, then a gap at once", mode, hz, gyro ? -45 : 0, 20,
						 hz >= 104 ? 0.6f : 2.5f);
			}
			/* the gyro's zero measured across a gap: 0.6 s still, 50 lost,
			 * 0.6 s still is a still second (the window keeps its samples) */
			if (gyro && hz >= 104) {
				struct input_motion_info mi;

				mm_no_bias = true;
				mm_start3(&p, &s, mode, hz, hz, id++, 0.6f);
				mm_no_bias = false;
				input_motion_info(&mi);
				CHECK(!mi.bias_known);
				ms_gap(&s, 50, 0, 0);
				ms_run(&s, ms_n(hz, 0.6f, 4), 0, 0);
				ms_feed(&p, &s);
				input_motion_info(&mi);
				if (!mi.bias_known) {
					fprintf(stderr, "matrix zero across a gap, %s, %g Hz: not known\n",
						mode == MM_GA ? "gyro+accel" : "gyro", (double)hz);
					failures++;
				}
			}
			/* a slow turn (smoothed: slower than the smoothing's 2.5 deg/s),
			 * then a gap: the smoothing's tail still comes after it (about
			 * 1.2 deg/s * 20 ms), what was turned is not lost */
			if (gyro && hz >= 104) {
				mm_start(&p, &s, mode, hz, id++);
				ms_run(&s, ms_n(hz, 0.4f, 4), 0, 1.2f);
				ms_feed(&p, &s);
				imu_turned = imu_my = 0;
				ms_gap(&s, 50, 0, 0);
				ms_run(&s, ms_n(hz, 0.05f, 4), 0, 0);
				ms_feed(&p, &s);
				if (!(imu_turned < -0.012f && imu_turned > -0.035f)) {
					fprintf(stderr, "matrix slow turn, then a gap, %s, %g Hz: tail %.4f\n",
						mode == MM_GA ? "gyro+accel" : "gyro", (double)hz,
						(double)imu_turned);
					failures++;
				}
			}

			/* tilted by 60 degrees (seen, or in a gap of 50 lost), then
			 * turned 45 degrees about the world's vertical: with gravity the
			 * view turns 45 (what was before the gap is stale: the "up" of
			 * then too); a gyro alone turns about the device's own vertical,
			 * cos 60 of it; an accelerometer alone sees no turn about gravity */
			for (int gi2 = 0; gi2 < 2; gi2++) {
				int nt = ms_n(hz, 1, 4);

				mm_start(&p, &s, mode, hz, id++);
				if (gi2) {
					ms_gap(&s, 50, 0, 60);	/* the turn right after */
				} else {
					ms_run(&s, ms_n(hz, 1, 4), 60 * hz / ms_n(hz, 1, 4), 0);
					ms_run(&s, ms_n(hz, 1, 3), 0, 0);	/* the tilt all told */
				}
				ms_feed(&p, &s);
				imu_turned = imu_my = 0;
				ms_run_vert(&s, nt, 45 * hz / nt);
				ms_run(&s, ms_n(hz, 2, 4), 0, 0);
				ms_feed(&p, &s);
				mm_check2(gi2 ? "tilted in a gap, turned about the vertical"
					      : "tilted, turned about the vertical", mode, hz,
					  mode == MM_GA ? -45 : mode == MM_G ? -22.5f : 0, 0,
					  hz >= 104 ? 0.6f : 2.5f, 1);
			}

			/* tilted by 60 degrees within one sample: the angle, not its sine */
			if (!gyro) {
				mm_start(&p, &s, mode, hz, id++);
				ms_run(&s, 1, 60 * hz, 0);
				ms_run(&s, ms_n(hz, 3, 4), 0, 0);
				ms_feed(&p, &s);
				mm_check("60 deg in one sample", mode, hz, 0, 60, 2);
			}

			for (unsigned gi = 0; gi < sizeof(gaps) / sizeof(gaps[0]); gi++) {
				/* samples lost while it turned: only what comes after counts */
				mm_start(&p, &s, mode, hz, id++);
				ms_gap(&s, gaps[gi], 0, 0);
				ms_run(&s, nturn, 0, w);
				ms_run(&s, ms_n(hz, 2, 3), 0, 0);
				ms_feed(&p, &s);
				mm_check("turn after lost samples", mode, hz, gyro ? -20 : 0, 0, 2.5f);
				/* samples lost while it tilted: never the view's */
				mm_start(&p, &s, mode, hz, id++);
				ms_gap(&s, gaps[gi], 0, 20);
				ms_run(&s, ms_n(hz, 3, 4), 0, 0);
				ms_feed(&p, &s);
				mm_check("tilt in lost samples", mode, hz, 0, 0, 1);
			}
			/* 3 samples lost while it tilted: an accelerometer alone sees it
			 * (a gyro decides the tilt where there is one: it saw none) */
			mm_start(&p, &s, mode, hz, id++);
			ms_gap(&s, 3, 0, 20);
			ms_run(&s, ms_n(hz, 3, 4), 0, 0);
			ms_feed(&p, &s);
			mm_check("tilt in 3 lost samples", mode, hz, 0, gyro ? 0 : 20, 2);
			/* 3 samples lost in the middle of a steady turn: no gap, the
			 * sample after counts their steps at its rate (the turn whole);
			 * at least 5 deg/s: a slower steady turn of a seconds-slow
			 * sensor is its gyro's zero as far as anyone can tell */
			{
				float r = fminf(fmaxf(20 * hz / 12, 5), 3000);	/* (int16 at 0.1 deg/s) */

				mm_start(&p, &s, mode, hz, id++);
				ms_run(&s, 8, 0, r);
				ms_gap(&s, 3, 0, 0);
				ms_run(&s, 1, 0, r);
				ms_run(&s, ms_n(hz, 3, 4), 0, 0);
				ms_feed(&p, &s);
				mm_check("3 lost in a steady turn", mode, hz, gyro ? -r * 12 / hz : 0, 0,
					 fmaxf(2.5f, r * 12 / hz * 0.05f));
			}
			/* time without samples, their numbers going on (the device set
			 * its sensor up again), a tilt in it, then a turn: up to 3.5
			 * periods it is no gap (2, 3, 3.45 periods; 0.5 s at 1 and 5 Hz),
			 * more is (3.55 and 10 periods; 0.5 s at 104 and 1000 Hz) */
			{
				static const float pauses[] = { 2, 3, 3.45f, 3.48f, 3.52f, 3.55f, 10, 0 };

				for (int pi = 0; pi < 8; pi++) {
					float extra = pauses[pi] > 0 ? pauses[pi] / hz : 0.5f;
					bool counted = extra * hz <= 3.5f && !gyro;

					mm_start(&p, &s, mode, hz, id++);
					ms_gap(&s, 0, extra, 20);
					ms_run(&s, nturn, 0, w);
					ms_run(&s, ms_n(hz, 3, 4), 0, 0);
					ms_feed(&p, &s);
					mm_check("tilt in a pause, then a turn", mode, hz, gyro ? -20 : 0,
						 counted ? 20 : 0, 2.5f);
				}
			}
			/* missing by number and by time together: one count (2 + 1 = 3
			 * is no gap, 3 + 3 = 6 is) */
			{
				static const int lost[2] = { 2, 3 }, pause[2] = { 1, 3 };

				for (int ci = 0; ci < 2; ci++) {
					bool counted = ci == 0 && !gyro;

					mm_start(&p, &s, mode, hz, id++);
					ms_gap(&s, lost[ci], pause[ci] / hz, 20);
					ms_run(&s, ms_n(hz, 3, 4), 0, 0);
					ms_feed(&p, &s);
					mm_check("lost and paused", mode, hz, 0, counted ? 20 : 0, 2);
				}
			}
			/* a data-ready missed now and then (a period with no sample
			 * after every 20th: one interval in 5 even with batches of 4;
			 * on the sensor's grid, as all its pauses short of a gap), 24
			 * s: not a slower rate, a turn while they go on as it was
			 * (each sample its own period: the missed one's time is no
			 * turn) */
			mm_start(&p, &s, mode, hz, id++);
			for (int r = 0; r < ms_n(hz, 24, 40) / 20; r++) {
				ms_run(&s, 20, 0, 0);
				ms_gap(&s, 0, 1 / hz, 0);
			}
			{
				int nt = ms_n(hz, 1, 4);

				imu_turned = imu_my = 0;
				for (int done = 0; done < nt; done += 20) {
					ms_run(&s, nt - done < 20 ? nt - done : 20, 0, 45 * hz / nt);
					ms_gap(&s, 0, 1 / hz, 0);
				}
				ms_run(&s, ms_n(hz, 2, 3), 0, 0);
				ms_feed(&p, &s);
				mm_check("pauses now and then, a turn", mode, hz, gyro ? -45 : 0, 0,
					 hz >= 104 ? 0.6f : 2.5f);
			}
			/* a slow sensor's few intervals: 3 lost + 0.4 period paused (no
			 * gap) does not move its rate, so 3 + 0.6 after it is a gap */
			mm_start2(&p, &s, mode, hz, hz * 1.0001f, id++);
			ms_gap(&s, 3, 0.4f / hz, 0);
			ms_run(&s, 3, 0, 0);
			ms_gap(&s, 3, 0.6f / hz, 20);
			ms_run(&s, ms_n(hz, 3, 4), 0, 0);
			ms_feed(&p, &s);
			mm_check("3+0.4, then 3+0.6", mode, hz, 0, 0, 1);
			/* two gaps close together, a tilt in each */
			mm_start(&p, &s, mode, hz, id++);
			ms_gap(&s, 6, 0, 20);
			ms_run(&s, ms_n(hz, 0.05f, 1), 0, 0);
			ms_gap(&s, 6, 0, 20);
			ms_run(&s, ms_n(hz, 3, 4), 0, 0);
			ms_feed(&p, &s);
			mm_check("two gaps close together", mode, hz, 0, 0, 1);
			/* a slow turn (smoothed), plugged out and in, tilted by 60
			 * meanwhile: a new parser, its numbers from anew; present only
			 * after the first read (a batch), as the driver says it. Its
			 * first sample is a gap, and no more: the tilt is not the
			 * view's, the smoothing's tail of the turn before still is */
			mm_start(&p, &s, mode, hz, id++);
			input_motion_present(true, hz);
			ms_run(&s, ms_n(hz, 0.4f, 4), 0, 1.2f);
			ms_feed(&p, &s);
			imu_turned = imu_my = 0;
			input_motion_present(false, 0);
			imu_parser_init(&p);
			snprintf(p.serial, sizeof(p.serial), "MM%u", id - 1);
			s.seq = 3;
			s.t += 5000000;
			s.tilt += 60;
			{
				uint8_t f[80];

				ms_put(&s, f, mm_info(f, mode, hz));
			}
			ms_run(&s, s.batch, 0, 0);
			ms_feed(&p, &s);
			input_motion_present(true, hz);
			ms_run(&s, ms_n(hz, 3, 4), 0, 0);
			ms_feed(&p, &s);
			input_motion_present(false, 0);
			mm_check("plugged in again", mode, hz, 0, 0, 1);
			if (gyro && hz >= 104 && !(imu_turned < -0.012f && imu_turned > -0.035f)) {
				fprintf(stderr, "matrix plugged in again, %s, %g Hz: tail %.4f\n",
					mode == MM_GA ? "gyro+accel" : "gyro", (double)hz, (double)imu_turned);
				failures++;
			}
			/* plugged out and in (tilted by 60 meanwhile), a turn about
			 * the world's vertical right after the first read: its zero
			 * kept, gravity anew (a gyro alone cos 60 of it); and a zero
			 * not known yet is measured across it (0.6 s + 0.6 s still) */
			for (int zi = 0; zi < (gyro && hz >= 104 ? 2 : 1); zi++) {
				uint8_t f[80];

				mm_no_bias = zi;
				mm_start3(&p, &s, mode, hz, hz, id++, zi ? 0.6f : 0);
				mm_no_bias = false;
				input_motion_present(true, hz);
				input_motion_present(false, 0);
				imu_parser_init(&p);
				snprintf(p.serial, sizeof(p.serial), "MM%u", id - 1);
				s.seq = 3;
				s.t += 5000000;
				s.tilt += zi ? 0 : 60;	/* (a tilt: not still, rightly) */
				ms_put(&s, f, mm_info(f, mode, hz));
				ms_run(&s, s.batch, 0, 0);
				ms_feed(&p, &s);
				input_motion_present(true, hz);
				if (zi) {
					struct input_motion_info mi;

					ms_run(&s, ms_n(hz, 0.6f, 4), 0, 0);
					ms_feed(&p, &s);
					input_motion_info(&mi);
					input_motion_present(false, 0);
					if (!mi.bias_known) {
						fprintf(stderr, "matrix zero across a reconnect, %s, %g Hz: not known\n",
							mode == MM_GA ? "gyro+accel" : "gyro", (double)hz);
						failures++;
					}
					continue;
				}
				imu_turned = imu_my = 0;
				{
					int nt = ms_n(hz, 1, 4);

					ms_run_vert(&s, nt, 45 * hz / nt);
				}
				ms_run(&s, ms_n(hz, 2, 4), 0, 0);
				ms_feed(&p, &s);
				input_motion_present(false, 0);
				mm_check("plugged in again, a turn at once", mode, hz,
					 mode == MM_GA ? -45 : mode == MM_G ? -22.5f : 0, 0, hz >= 104 ? 0.6f : 2.5f);
			}
		}
	}
	/* the samples' times as a real one's: found late by a delay that creeps
	 * (the firmware's polling beats with the sensor's clock) and wraps; the
	 * intervals are skewed (most a little long, a few short): the period
	 * is their long-run mean, not their median */
	/* the delay creeps slowly (7 us a batch, wraps at 200 us: every ~100
	 * ms) or fast (70 us, wraps at 350 us: every 5 batches, as wide as on
	 * the real sensor), or every other batch is found 270 us late (two
	 * intervals' times apart by twice the delay's spread); and now and
	 * then a frame's time is found 0.9 ms late, and a data-ready is missed
	 * (a period more, no number for it): a window's intervals hold
	 * no wrap or a few, so only a measure over several windows is right.
	 * Within 0.3 % from the first measure on (a pause in the first window
	 * takes its interval's delay out), 0.15 % after the first second (a
	 * late time takes its two intervals' share of the delay out), all
	 * along, a second without samples and 900 lost in it in 8 places each
	 * (in the first window too) */
	for (int ji = 0; ji < 3; ji++)
		for (int pk = 0; pk < 8; pk++) {
			static const int pause_b[8] = { 5, 33, 70, 1000, 1006, 1015, 1500, 1501 };
			static const int lost_b[8] = { 12, 50, 2000, 2007, 2013, 2025, 6, 9 };
			const float real_us = 1e6f / 1137.0f;
			const uint32_t creep = ji == 1 ? 70 : 7, wrap = ji == 1 ? 350 : ji == 2 ? 1 : 200;
			uint32_t delay = 0, base;
			float worst = 0, worst1 = 0;

			mm_start3(&p, &s, MM_GA, 1125, 1137, id++, 0.01f);
			base = s.t;
			for (int b = 0; b < 3000; b++) {	/* 12000 samples, 10.6 s */
				uint32_t at;

				if (b == pause_b[pk])
					base += 1000000;
				if (b == lost_b[pk]) {
					base += (uint32_t)lrintf(900 * real_us);
					s.seq = (uint16_t)(s.seq + 900);
				}
				if (b % 97 == 13)
					base += (uint32_t)lrintf(real_us);	/* a data-ready missed */
				at = base + (uint32_t)lrintf(4 * b * real_us);
				delay = ji == 2 ? (uint32_t)(b % 2) * 270u : (delay + creep) % wrap;
				s.t = at + delay + (b % 50 == 25 ? 900 : 0);
				ms_run(&s, 4, 0, 0);
				if (b % 10 == 9) {
					ms_feed(&p, &s);
					if (p.period_measured) {
						float off = fabsf(p.period_us - real_us) / real_us;

						worst = fmaxf(worst, off);
						if (b >= 285)	/* a second on */
							worst1 = fmaxf(worst1, off);
					}
				}
			}
			if (!p.period_measured || worst >= 0.003f || worst1 >= 0.0015f) {
				fprintf(stderr, "matrix jittered times (model %d, place %d): off by %.3f %%, "
					"%.3f %% after a second\n", ji, pk, (double)worst * 100,
					(double)worst1 * 100);
				failures++;
			}
		}
	/* the jittered stream with nothing in it: the first window within
	 * 0.15 % (the delay's spread over a quarter second), 0.05 % after a
	 * second; and a data-ready missed right where the creeping delay wraps
	 * (the interval 879 - 280 us beyond its numbers: the closest a missed
	 * period comes to the jitter) in the first window: out, within 0.3 %.
	 * A window with 900 lost or a second without samples in it is as long:
	 * its first measure after 0.22 s of samples at least */
	for (int ci = 0; ci < 6; ci++) {
		const float real_us = 1e6f / 1137.0f;
		const int model = ci < 3 ? ci : 1;
		uint32_t delay = 0, base, had = 0;
		float worst = 0, worst1 = 0;

		mm_start3(&p, &s, MM_GA, 1125, 1137, id++, 0.001f);
		base = s.t;
		for (int b = 0; b < 1500; b++) {
			uint32_t at;

			if (ci == 3 && b == 19)
				base += (uint32_t)lrintf(real_us);	/* missed, as the delay wraps */
			if (ci == 4 && b == 5) {
				base += (uint32_t)lrintf(900 * real_us);
				s.seq = (uint16_t)(s.seq + 900);
			}
			if (ci == 5 && b == 5)
				base += 1000000;
			at = base + (uint32_t)lrintf(4 * b * real_us);
			delay = model == 2 ? (uint32_t)(b % 2) * 270u : (delay + (model ? 70 : 7)) % (model ? 350 : 200);
			s.t = at + delay;
			ms_run(&s, 4, 0, 0);
			ms_feed(&p, &s);
			if (p.period_measured) {
				float off = fabsf(p.period_us - real_us) / real_us;

				if (!had)
					had = p.samples;
				worst = fmaxf(worst, off);
				if (b >= 285)
					worst1 = fmaxf(worst1, off);
			}
		}
		if (worst >= (ci < 3 ? 0.0015f : 0.003f) || worst1 >= (ci < 3 ? 0.0005f : 0.0015f) ||
		    (ci >= 4 && had < 0.22f * 1137)) {
			fprintf(stderr, "jitter edge case %d: off by %.3f %%, %.3f %% after a second, "
				"measured after %u samples\n", ci, (double)worst * 100, (double)worst1 * 100, had);
			failures++;
		}
	}
	/* the info changed in one thing only (its name, its id, a channel,
	 * the magnetometer's or the temperature's bit, a unit, a noise): a new
	 * stream each (told, measured anew); the same info: none. And a stream
	 * numbered from 0, as the firmware's: its first frame counts */
	for (int vi = 0; vi <= 9; vi++) {
		static const char *const names[2] = { "MATRIX", "MATRIY" };
		uint8_t f[80];
		unsigned was;

		mm_start(&p, &s, MM_GA, 1000, id++);
		was = p.info_seq;
		ms_flush(&s);
		ms_put(&s, f, imu_info(f, names[vi == 1], vi == 2 ? 10 : 9,
				       (uint8_t)(vi == 3 ? 1 : 3 | (vi == 4 ? 4 : 0) | (vi == 5 ? 8 : 0)), 1000000,
				       vi == 6 ? 0.2f : 0.1f, vi == 7 ? 2.0f / 8192 : 1.0f / 8192,
				       vi == 8 ? 0.2f : 0.1f, vi == 9 ? 0.003f : 0.002f));
		ms_run(&s, 8, 0, 0);
		ms_feed(&p, &s);
		if (vi == 0)
			CHECK(p.info_seq == was && p.period_measured);
		else if (!(p.info_seq == was + 1 && !p.period_measured)) {
			fprintf(stderr, "info changed in field %d: seq %u -> %u, measured %d\n", vi, was,
				p.info_seq, p.period_measured);
			failures++;
		}
	}
	{
		uint8_t f[80];

		input_set_motion_calibration(NULL);
		imu_parser_init(&p);
		snprintf(p.serial, sizeof(p.serial), "MM%u", id++);
		imu_parser_feed(&p, f, mm_info(f, MM_GA, 1000));
		s.len = 0;
		s.pending = 0;
		s.seq = 0;
		s.hz = 1000;
		s.batch = 4;
		ms_run(&s, 8, 0, 0);
		ms_feed(&p, &s);
		CHECK(p.samples == 8);
	}
	/* what the driver logs of the rate (imu_rate_news()): refused once;
	 * a new info does not tell the old refusal; its measure, once; a
	 * change within 0.5..2x nothing; beyond: refused, back: measured;
	 * each new info's measure anew */
	{
		struct imu_rate_told told = { 0 };
		uint8_t f[80];

		mm_start2(&p, &s, MM_GA, 1000, 300, id++);
		CHECK(imu_rate_news(&p, &told) == IMU_RATE_REFUSED && imu_rate_news(&p, &told) == 0);
		ms_flush(&s);
		ms_put(&s, f, mm_info(f, MM_GA, 2000));
		s.hz = 2000;
		s.batch = 4;
		ms_run(&s, 200, 0, 0);		/* 0.1 s: not measured yet */
		ms_feed(&p, &s);
		CHECK(imu_rate_news(&p, &told) == 0);
		ms_run(&s, 1000, 0, 0);
		ms_feed(&p, &s);
		CHECK(imu_rate_news(&p, &told) == IMU_RATE_MEASURED && imu_rate_news(&p, &told) == 0);
		CHECK(fabsf(p.period_us - 500) < 1);
		s.hz = 3000;
		ms_run(&s, 9000, 0, 0);
		ms_feed(&p, &s);
		CHECK(imu_rate_news(&p, &told) == 0 && p.period_measured && fabsf(p.period_us - 333) < 1);
		s.hz = 8000;
		ms_run(&s, 8000, 0, 0);
		ms_feed(&p, &s);
		CHECK(imu_rate_news(&p, &told) == IMU_RATE_REFUSED && fabsf(p.period_us - 500) < 1);
		s.hz = 2000;
		ms_run(&s, 1200, 0, 0);
		ms_feed(&p, &s);
		CHECK(imu_rate_news(&p, &told) == IMU_RATE_MEASURED && fabsf(p.period_us - 500) < 1);
		/* another info, measured too: told for it */
		ms_flush(&s);
		ms_put(&s, f, mm_info(f, MM_GA, 1000));
		s.hz = 1000;
		ms_run(&s, 600, 0, 0);
		ms_feed(&p, &s);
		CHECK(imu_rate_news(&p, &told) == IMU_RATE_MEASURED && fabsf(p.period_us - 1000) < 1);
	}
	input_set_motion_calibration(NULL);

	free(s.buf);
	input_deinit();
	mainloop_free(ml);

	/* the driver: a sensor is live while its samples came within 1.5 s or 3
	 * of its periods (a slow one is not "gone" between two samples) */
	CHECK(imu_live(10000, 8500, 5, 1000) && !imu_live(10000, 8499, 5, 1000));	/* 1.5 s */
	CHECK(imu_live(10000, 7000, 5, 1e6f) && !imu_live(10000, 6999, 5, 1e6f));	/* 3 periods */
	CHECK(!imu_live(10000, 10000, 0, 1000));	/* none yet */
	/* ...and says once that they stopped: live, gone (said), still gone
	 * (not again), back, gone again (said) */
	{
		bool live = false;

		CHECK(!imu_watch(&live, 1000, 900, 5, 1000) && live);
		CHECK(imu_watch(&live, 3000, 900, 5, 1000) && !live);
		CHECK(!imu_watch(&live, 4000, 900, 5, 1000) && !live);
		CHECK(!imu_watch(&live, 5000, 4900, 9, 1000) && live);
		CHECK(imu_watch(&live, 7000, 4900, 9, 1000) && !live);
	}
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
	CHECK(fabsf(p.period_us - 1000) < 1);	/* measured by the sensor's clock */
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
		input_motion_sensor(&(struct input_motion_desc){ "AAA-1", "AAA", 3, 0.15f, 0.0022f, 1000 });
		CHECK(!input_motion_calibration(&c));
		CHECK(fabsf(input_motion()->still_gyro - 0.6f) < 1e-5f &&
		      fabsf(input_motion()->still_accel - 0.0088f) < 1e-6f);
		CHECK(fabsf(input_motion()->smooth_dps - 3.75f) < 1e-5f &&
		      fabsf(input_motion()->tight_dps - 0.3f) < 1e-6f);
		input_motion_sensor(&(struct input_motion_desc){ "FAKE-X1", "FAKE", 3, 0.12f, 0, 1000 });
		CHECK(input_motion_calibration(&c) && c.right[1] == 1 && !c.bias_set);
		CHECK(!input_motion()->bias_known && input_motion()->still_accel == MOTION_STILL_ACCEL);
		input_motion_sensor(&(struct input_motion_desc){ "FAKE-X2", "FAKE", 3, 0, 0, 1000 });
		CHECK(!input_motion_calibration(&c) && input_motion()->still_gyro == MOTION_STILL_GYRO);
		CHECK(input_motion()->smooth_dps == MOTION_SMOOTH_DPS);
		input_set_motion_calibration(&(struct motion_cal){ .bias_set = true, .bias = { 1, 2, 3 } });
		input_motion_sensor(&(struct input_motion_desc){ "AAA-1", "AAA", 3, 0.15f, 0.0022f, 1000 });
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
		input_motion_sensor(&(struct input_motion_desc){ "FAKE-X2", "FAKE", 3, 0, 0, 1000 });
		CHECK(input_motion_calibration(&c) && c.bias[2] == 3);
		unlink("test_cal.conf");
		input_set_motion_calibration(NULL);
	}

	/* a sensor without an accelerometer: its own axes, from the calibration */
	{
		struct input_motion_info mi;
		struct motion_cal c = { .axes_set = true, .right = { 1, 0, 0 }, .up_set = true, .up = { 0, 0, 1 } };

		input_motion_sensor(&(struct input_motion_desc){ "GYRO-1", "GYRO", MOTION_CH_GYRO, 0.15f, 0, 1000 });
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
		input_motion_sensor(&(struct input_motion_desc){ "ACC-1", "ACC", MOTION_CH_ACCEL, 0, 0.002f, 1000 });
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
		input_motion_sensor(&(struct input_motion_desc){ "ACC-2", "ACC", MOTION_CH_ACCEL, 0, 0.002f, 1000 });
		input_set_motion_calibration(&(struct motion_cal){ .axes_set = true, .right = { 1, 0, 0 } });
		input_motion_present(true, 1000);
		imu_feed(2000, 0, 0, 0, 0, 0);
		input_motion_present(false, 0);
		/* input_motion_gap() itself (the parser calls it on a new stream:
		 * the matrix tests that path), then 40 samples, then present */
		input_motion_gap();
		imu_my = 0;
		imu_feed(40, 0, 0, 0, 60, 60);
		input_motion_present(true, 1000);
		imu_feed(1960, 0, 0, 0, 60, 60);
		CHECK(fabsf(imu_my) < 1);
		imu_feed(500, 0, 0, 0, 0, 0);	/* the gap in the stream */
		input_motion_gap();
		imu_my = 0;
		imu_feed(2000, 0, 0, 0, 60, 60);
		CHECK(fabsf(imu_my) < 1);
		input_motion_present(false, 0);
		input_set_motion_calibration(NULL);
	}

	/* no gyro, gaps in the stream (samples lost, or none for a while with
	 * their numbers going on) while the device tilts by 20 degrees each
	 * time: the turn made meanwhile is never the view's (no jump) */
	{
		/* rate, then segments: seconds of samples, and before them a gap of
		 * lost samples (seconds) or of time alone (seconds) */
		static const struct { float hz; float seg[3][3]; } sc[] = {
			{ 1000, { { 1, 0, 0 }, { 1, 0.05f, 0 } } },
			{ 1000, { { 1, 0, 0 }, { 1, 0.2f, 0 } } },
			{ 1000, { { 1, 0, 0 }, { 1, 0.5f, 0 } } },
			{ 1000, { { 1, 0, 0 }, { 1, 0.9f, 0 } } },
			/* two gaps close together */
			{ 1000, { { 1, 0, 0 }, { 0.1f, 0.05f, 0 }, { 1, 0.05f, 0 } } },
			{ 1000, { { 1, 0, 0 }, { 0.05f, 0.09f, 0 }, { 1, 0.02f, 0 } } },
			{ 1000, { { 1, 0, 0 }, { 0.3f, 0.05f, 0 }, { 1, 0.05f, 0 } } },
			{ 104, { { 2, 0, 0 }, { 0.5f, 0.09f, 0 }, { 2, 0.09f, 0 } } },
			{ 104, { { 2, 0, 0 }, { 1, 0.09f, 0 }, { 2, 0.06f, 0 } } },
			/* the numbers go on, the time jumps (the sensor set up again) */
			{ 1000, { { 1, 0, 0 }, { 1, 0, 0.2f } } },
			{ 1000, { { 1, 0, 0 }, { 1, 0, 1.1f } } },
			{ 1000, { { 1, 0, 0 }, { 1, 0, 1.5f } } },
		};

		for (unsigned si = 0; si < sizeof(sc) / sizeof(sc[0]); si++) {
			uint8_t hdr[8 + 12], st3[36] = { 3, 0, 3, 0x0b };
			uint16_t sq = 100;
			uint32_t t = 0, per = (uint32_t)lrintf(1e6f / sc[si].hz);
			float tilt = 0;
			size_t m = 0;

			imu_parser_init(&p);
			snprintf(p.serial, sizeof(p.serial), "GAP%u", si);
			m += imu_info(stream + m, "ACCONLY", 9, 2, (uint32_t)(sc[si].hz * 1000), 0, 1.0f / 8192,
				      0, 0.002f);
			m += imu_frame(stream + m, 0x02, st3, 36);
			/* the sensor known first, then its calibration */
			imu_parser_feed(&p, stream, m);
			CHECK(p.channels == 2 && input_motion()->channels == MOTION_CH_ACCEL);
			input_set_motion_calibration(&(struct motion_cal){ .axes_set = true,
									   .right = { 1, 0, 0 } });
			m = 0;
			for (int sg = 0; sg < 3 && sc[si].seg[sg][0] > 0; sg++) {
				int16_t ay = (int16_t)lrintf(sinf(tilt * 0.017453293f) * 8192);
				int16_t az = (int16_t)lrintf(cosf(tilt * 0.017453293f) * 8192);
				int ns = (int)lrintf(sc[si].seg[sg][0] * sc[si].hz);

				sq = (uint16_t)(sq + lrintf(sc[si].seg[sg][1] * sc[si].hz));
				t += (uint32_t)lrintf((sc[si].seg[sg][1] + sc[si].seg[sg][2]) * 1e6f);
				for (int i = 0; i < ns; i++) {
					memcpy(hdr, &t, 4);
					memcpy(hdr + 4, &sq, 2);
					hdr[6] = 1;
					hdr[7] = 0;
					memset(hdr + 8, 0, 12);
					memcpy(hdr + 8 + 8, &ay, 2);
					memcpy(hdr + 8 + 10, &az, 2);
					m += imu_frame(stream + m, 0x04, hdr, 8 + 12);
					sq++;
					t += per;
				}
				tilt += 20;	/* during the next gap */
			}
			imu_my = 0;
			imu_parser_feed(&p, stream, m);
			CHECK(input_motion()->cal.axes_set && fabsf(imu_my) < 1);
			if (fabsf(imu_my) >= 1)
				fprintf(stderr, "gap scenario %u: the view moved %.2f deg\n", si, (double)imu_my);
			input_set_motion_calibration(NULL);
		}
	}

	/* no gyro, plugged out and in again (a new port: a new parser), turned
	 * 60 degrees meanwhile; the driver says it is present only after its
	 * first read, here all of the samples: no jump */
	{
		uint16_t sq = 10;
		uint32_t t = 0;
		size_t m;

		imu_acc_start(&p, stream, "RECON", 1000);
		input_motion_present(true, 1000);
		m = imu_acc_samples(stream, &sq, &t, 1000, 1000, 0, 0);
		imu_parser_feed(&p, stream, m);
		input_motion_present(false, 0);
		imu_parser_init(&p);	/* open_port() */
		snprintf(p.serial, sizeof(p.serial), "RECON");
		sq = 7;			/* the device started its numbers again */
		t = 123456;
		imu_my = 0;
		m = imu_info(stream, "ACCONLY", 9, 2, 1000000, 0, 1.0f / 8192, 0, 0.002f);
		m += imu_acc_samples(stream + m, &sq, &t, 1000, 1500, 60, 60);
		imu_parser_feed(&p, stream, m);
		input_motion_present(true, 1000);
		CHECK(input_motion()->cal.axes_set && fabsf(imu_my) < 1);
		input_motion_present(false, 0);
		input_set_motion_calibration(NULL);
	}

	/* slow sensors without a gyro: a tilt of 20 degrees over 4 s is the
	 * view's (no step cut short, no "gap" at every sample) */
	{
		static const float hz[] = { 1, 2, 5 };

		for (unsigned hi = 0; hi < sizeof(hz) / sizeof(hz[0]); hi++) {
			uint16_t sq = 1;
			uint32_t t = 0;
			size_t m;
			char serial[16];

			snprintf(serial, sizeof(serial), "SLOW%u", hi);
			imu_acc_start(&p, stream, serial, hz[hi]);
			m = imu_acc_samples(stream, &sq, &t, hz[hi], (int)(4 * hz[hi]), 0, 0);
			imu_parser_feed(&p, stream, m);
			imu_my = 0;
			m = imu_acc_samples(stream, &sq, &t, hz[hi], (int)(4 * hz[hi]), 0, 20);
			m += imu_acc_samples(stream + m, &sq, &t, hz[hi], (int)(6 * hz[hi]), 20, 20);
			imu_parser_feed(&p, stream, m);
			CHECK(fabsf(imu_my - 20) < 2);
			if (fabsf(imu_my - 20) >= 2)
				fprintf(stderr, "%.0f Hz: the view moved %.2f deg of 20\n", (double)hz[hi],
					(double)imu_my);
			input_set_motion_calibration(NULL);
		}
	}

	/* the sensor's rate from its info reaches the made-up gyro: its noise
	 * estimate, so the thresholds, follow a new rate */
	{
		float still_fast, still_slow;
		size_t m;

		imu_acc_start(&p, stream, "RATE", 1000);
		still_fast = input_motion()->still_gyro;
		m = imu_info(stream, "ACCONLY", 9, 2, 104000, 0, 1.0f / 8192, 0, 0.002f);
		imu_parser_feed(&p, stream, m);
		still_slow = input_motion()->still_gyro;
		CHECK(still_fast > 0.9f && still_fast < 1.3f && still_slow > still_fast);
		input_set_motion_calibration(NULL);
	}

	/* neither: nothing to aim with, the samples are not used */
	{
		struct input_motion_info mi;

		input_motion_sensor(&(struct input_motion_desc){ "MAG-1", "MAG", 0, 0, 0, 1000 });
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
	test_motion_matrix();
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
