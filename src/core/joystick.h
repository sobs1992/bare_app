/* Analog stick math, no I/O: easy to test on the host. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* An axis as the driver reports it */
struct joy_axis_cal {
	int min, max;		/* raw range reported by the driver */
	bool invert;
};

/* Raw value -> -1..1 over the driver's whole range (its middle is 0) */
float joy_linear(const struct joy_axis_cal *c, int raw);

/*
 * A stick as a whole, in joy_linear() units. The rest position is taken
 * away, each half axis is scaled to its own reach (left, right, up and
 * down differ), then the edge the stick really reaches in each direction
 * becomes the unit circle; a round dead zone around the center follows.
 * Without a calibration the reaches are the driver's range from the rest
 * position and the edge is taken as round.
 */
#define JOY_SECTORS 16		/* edge directions, sector k at k * 360 / 16 degrees */

struct joy_stick_cal {
	float cx, cy;		/* rest position */
	float xn, xp, yn, yp;	/* reach from it: left, right, up, down (> 0) */
	float edge[JOY_SECTORS];	/* after that scaling: the edge's distance (1 = round) */
};

void joy_stick_default(struct joy_stick_cal *c, float rest_x, float rest_y);
/*
 * The dead zone is round, deadzone wide; deadzone_down widens its lower
 * half (a stick that a light touch from above tips down): that wide
 * straight down, narrowing as the sine of the angle squared to deadzone
 * at the sides. Below deadzone it changes nothing. Returns the stick's
 * distance from the rest position before the dead zone is taken away
 * (1 = the edge).
 */
float joy_stick_apply(const struct joy_stick_cal *c, float deadzone, float deadzone_down,
		      float x, float y, float *out_x, float *out_y);
/* Sensitivity: a stick's position (out of joy_stick_apply()) times gain,
 * cut at the unit circle */
void joy_stick_gain(float gain, float *x, float *y);

/*
 * Snap-back: a stick whose spring is weak jumps most of the way home when
 * let go, then creeps the rest slowly, and a game keeps moving all that
 * while. joy_snap_update() takes each new distance from the rest position
 * (calibrated, 0..1) with its time; after a fast fall towards the rest
 * position (JOY_SNAP_DROP within JOY_SNAP_WINDOW_MS, ending below
 * JOY_SNAP_LEFT of where it fell from) it says "at rest" (true) until the
 * stick goes out again JOY_SNAP_REARM past the nearest point since.
 */
#define JOY_SNAP_WINDOW_MS 150
#define JOY_SNAP_DROP 0.35f
#define JOY_SNAP_LEFT 0.6f
#define JOY_SNAP_REARM 0.08f
#define JOY_SNAP_SAMPLES 32

struct joy_snap {
	bool held;		/* the fall was seen: at rest */
	float low;		/* nearest to the rest position since */
	unsigned head, count;
	float r[JOY_SNAP_SAMPLES];
	unsigned long long t[JOY_SNAP_SAMPLES];
};

bool joy_snap_update(struct joy_snap *s, unsigned long long now_ms, float r);

/*
 * Touch filter: a finger resting on a light stick tips it out of the dead
 * zone for a moment and lets it go back (measured on the OGA: up to 0.46
 * of the reach, out for 15..210 ms, mostly under 100). A stick pushed on
 * purpose either goes far at once or stays out. joy_touch_update() takes
 * each reading: r from joy_stick_apply() and whether it is past the dead
 * zone. It passes the stick (true) once it has been out JOY_TOUCH_MS
 * without a break, or at once when it gets JOY_TOUCH_FAST far, and then
 * until it is back in the dead zone. While it holds the stick back it
 * says in *wait_ms when to ask again with the same reading (a stick held
 * still sends nothing); 0 otherwise.
 */
#define JOY_TOUCH_MS 100
#define JOY_TOUCH_FAST 0.5f

struct joy_touch {
	bool open;		/* passing */
	bool out;		/* past the dead zone since 'since', not passed yet */
	unsigned long long since;
};

bool joy_touch_update(struct joy_touch *t, unsigned long long now_ms, float r, bool out,
		      unsigned *wait_ms);

/* "cx cy xn xp yn yp edge0 .. edge15", for a settings file */
int joy_stick_format(const struct joy_stick_cal *c, char *buf, size_t size);
bool joy_stick_parse(const char *s, struct joy_stick_cal *c);

/*
 * Calibration: the rest position first, then the stick turned along its
 * edge. The farthest point of each of JOY_CAL_BINS directions is kept.
 */
#define JOY_CAL_BINS 64

struct joy_cal_rec {
	float cx, cy;
	float xmin, xmax, ymin, ymax;	/* of the position less the rest one */
	float bx[JOY_CAL_BINS], by[JOY_CAL_BINS], br[JOY_CAL_BINS];	/* br 0: none */
};

void joy_cal_begin(struct joy_cal_rec *r, float cx, float cy);
void joy_cal_add(struct joy_cal_rec *r, float x, float y);
/* Directions (of JOY_SECTORS) where the edge was reached */
int joy_cal_covered(const struct joy_cal_rec *r);
/* false: the edge was not gone round, c untouched */
bool joy_cal_finish(const struct joy_cal_rec *r, struct joy_stick_cal *c);

/*
 * Direction with hysteresis: prev is -1/0/+1, returns the new one.
 * A direction turns on beyond 'press' and off below 'release'.
 */
int joy_direction(int prev, float v, float press, float release);
