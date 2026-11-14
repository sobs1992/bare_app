/*
 * Motion sensor math (gyro aiming), no I/O: easy to test on the host.
 *
 * A gyroscope and an accelerometer fixed to the device, in their own axes
 * (the sensor's, whichever way it is mounted), turn into the turn of a
 * game's view: degrees to the right (x) and down (y), as a mouse moves.
 *
 *  - The gyro's zero (bias) drifts with time and temperature: it is
 *    measured whenever the device lies still (MOTION_STILL_MS of samples
 *    moving less than the noise), and by the calibration.
 *  - "Up" comes from gravity: the accelerometer, kept steady through quick
 *    moves by turning it with the gyro (a complementary filter).
 *  - Turning left and right is the turn about the world's vertical, however
 *    the device is tilted (player space): the same for a device held
 *    upright or lying flat, for every way the sensor is mounted.
 *  - Looking up and down is the turn about the device's own left-right
 *    axis, which only the calibration knows (the sensor's mounting): without
 *    it there is no up and down. The top tilted towards the player looks
 *    down, away from the player up.
 *  - Small turns (a hand's tremor) are smoothed, big ones go as they are.
 *
 * Nothing is assumed of the sensor: it says which channels it has and how
 * noisy they are (motion_set_sensor(), from the device's PKT_INFO), and the
 * thresholds follow its noise.
 *  - Without an accelerometer there is no gravity: left and right is the turn
 *    about the device's own vertical, which the calibration finds as well
 *    (motion_cal.up), and "still" is the gyro's alone.
 *  - Without a gyro motion_synth_gyro() makes one from the turn of gravity:
 *    up and down (and a roll) work, the turn about the vertical cannot be
 *    seen.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MOTION_STILL_MS 1000		/* still that long: the bias is measured */
#define MOTION_STILL_GYRO 0.6f		/* deg/s: the gyro's spread while still (noise unknown) */
#define MOTION_STILL_NOISE 4.0f		/* ...else this many times its noise, within: */
#define MOTION_STILL_MIN 0.3f
#define MOTION_STILL_MAX 1.5f
#define MOTION_STILL_ACCEL 0.01f	/* g: ...and the accelerometer's (noise unknown), else: */
#define MOTION_STILL_ACCEL_MIN 0.003f
#define MOTION_STILL_ACCEL_MAX 0.03f
#define MOTION_BIAS_MAX 10.0f		/* deg/s: a bigger "bias" is a turn */
#define MOTION_BIAS_STEP 2.0f		/* deg/s: ...and so is a bigger change of it */
#define MOTION_GRAVITY_S 0.5f		/* the accelerometer pulls "up" in about this */
#define MOTION_SMOOTH_DPS 4.0f		/* turns slower than this are smoothed (noise unknown)... */
#define MOTION_SMOOTH_NOISE 25.0f	/* ...else this many times the gyro's noise, within: */
#define MOTION_SMOOTH_MIN 2.0f
#define MOTION_SMOOTH_MAX_DPS 8.0f
#define MOTION_SMOOTH_S 0.04f		/* ...over that much time */
#define MOTION_SMOOTH_MAX 512		/* samples kept for it: 40 ms up to 12.8 kHz */
#define MOTION_TIGHT_DPS 0.3f		/* slower still: scaled down (noise unknown), else: */
#define MOTION_TIGHT_NOISE 2.0f
#define MOTION_TIGHT_MIN 0.1f
#define MOTION_TIGHT_MAX 0.6f
#define MOTION_SYNTH_S 0.03f		/* no gyro: gravity smoothed over this, then its turn */

/* The sensor's channels (as the protocol's PKT_INFO.channels) */
#define MOTION_CH_GYRO 1u
#define MOTION_CH_ACCEL 2u
#define MOTION_YAW_RELAX 1.41f		/* player space: a tilted device turns as well */

/* What the calibration found (the bias also comes by itself when still) */
struct motion_cal {
	bool bias_set;
	float bias[3];		/* deg/s */
	bool axes_set;
	float right[3];		/* unit: the device's right in the sensor's axes */
	bool up_set;		/* found only without an accelerometer */
	float up[3];		/* unit: the device's vertical, a turn to the left about it is + */
};

struct motion {
	struct motion_cal cal;
	unsigned channels;	/* MOTION_CH_* */
	float bias[3];		/* in use */
	bool bias_known;	/* measured (still) or calibrated */
	float up[3];		/* unit, sensor axes */
	bool up_set;
	/* stillness: sums of the samples since 'still_t' started */
	double still_g[3], still_g2[3], still_a[3], still_a2[3];
	float still_t;
	unsigned still_n;
	/* smoothing of small turns: rates (deg/s) of the last samples */
	float sm_x[MOTION_SMOOTH_MAX], sm_y[MOTION_SMOOTH_MAX], sm_dt[MOTION_SMOOTH_MAX];
	unsigned sm_head, sm_count;
	float x, y;		/* degrees turned, not taken yet */
	bool still;		/* the last window was still */
	float still_gyro;	/* deg/s: the spread of a still window (motion_set_sensor()) */
	float still_accel;	/* g: likewise */
	float smooth_dps, tight_dps;	/* the smoothing's thresholds */
	float rate_x, rate_y;	/* deg/s of the last sample, after the bias */
};

void motion_init(struct motion *m);
/* The calibration, NULL: none (the bias then comes when the device lies still) */
void motion_set_cal(struct motion *m, const struct motion_cal *c);
/*
 * One sample, in the sensor's axes: the gyro in deg/s, the accelerometer
 * in g, dt seconds since the sample before (gaps included).
 */
void motion_feed(struct motion *m, const float gyro[3], const float accel[3], float dt);
/* What the sensor has (MOTION_CH_*) and its noise at rest (deg/s and g rms,
 * 0 unknown): what "still" is, what is smoothed. motion_init(): both channels,
 * noise unknown. */
void motion_set_sensor(struct motion *m, unsigned channels, float gyro_rms, float accel_rms);
/* Does the sensor give anything to aim with (and what): with the calibration
 * as it is now, can it turn the view left and right, up and down? */
void motion_can_aim(const struct motion *m, bool *x, bool *y);

/* A gyro made from the accelerometer (a sensor without one): gravity smoothed,
 * its turn per sample; deg/s out, about the axes across gravity only */
struct motion_synth {
	float up[3];
	bool set;
};
void motion_synth_gyro(struct motion_synth *s, const float accel[3], float dt, float gyro[3]);
/* Degrees turned since the last call: x to the right, y down (as a mouse) */
void motion_take(struct motion *m, float *x, float *y);
/* Forget what was turned and not taken (the game was not in front) */
void motion_drop(struct motion *m);

/*
 * Calibration: first the device lies still (the bias), then it is held as
 * in a game and its top tilted towards the player: the turn's axis is the
 * device's right. Without an accelerometer it is then turned to the left as
 * well: that axis, less the right, is its vertical (motion_rec_tilt_begin()
 * with the right as 'up', motion_rec_right() gives it). motion_rec_* collect them.
 */
struct motion_rec {
	double g[3];		/* still: sums */
	unsigned n;
	float spread_g, spread_a;	/* still: the most each axis moved */
	float gmin[3], gmax[3], amin[3], amax[3];
	float up[3];		/* tilt: "up" when it started */
	double turn[3];		/* tilt: the turn so far (degrees, sensor axes) */
};

void motion_rec_begin(struct motion_rec *r);
/* Still: false once it moved too much (start again) */
bool motion_rec_still(struct motion_rec *r, const float gyro[3], const float accel[3]);
bool motion_rec_bias(const struct motion_rec *r, float bias[3]);	/* false: no samples */
/* Tilt: from 'up' (motion's at the start; zero without one), with the bias
 * known; the degrees turned about the axis found so far */
void motion_rec_tilt_begin(struct motion_rec *r, const float up[3]);
float motion_rec_tilt(struct motion_rec *r, const float gyro[3], const float bias[3], float dt);
/* The right axis: false if the turn was mostly about "up" (turned, not tilted) */
bool motion_rec_right(const struct motion_rec *r, float right[3]);

/* "bias x y z right x y z up x y z" for a settings file; parse takes what is there */
int motion_cal_format(const struct motion_cal *c, char *buf, size_t size);
bool motion_cal_parse(const char *s, struct motion_cal *c);
