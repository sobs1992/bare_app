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
void joy_stick_apply(const struct joy_stick_cal *c, float deadzone, float x, float y,
		     float *out_x, float *out_y);
/* Sensitivity: a stick's position (out of joy_stick_apply()) times gain,
 * cut at the unit circle */
void joy_stick_gain(float gain, float *x, float *y);

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
