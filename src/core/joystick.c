#include "joystick.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TWO_PI 6.28318531f
#define SECTOR (TWO_PI / JOY_SECTORS)
#define MIN_REACH 0.2f		/* a half axis that moves less is not a stick */
#define BIN_MIN 0.3f		/* nearer the rest position: not the edge */
#define EDGE_SEEN 0.8f		/* of the reach: a direction counts as gone round */
#define EDGE_MAX_GAP (2 * SECTOR)
/* The edge a little inside the farthest point seen: the stick pushed to
 * its edge gives 1 every time, not 0.97 once and 1 the other */
#define EDGE_MARGIN 0.96f

float joy_linear(const struct joy_axis_cal *c, int raw)
{
	float half = (float)(c->max - c->min) / 2, v;

	if (half <= 0)
		return 0;
	v = ((float)raw - (float)(c->min + c->max) / 2) / half;
	if (v > 1)
		v = 1;
	if (v < -1)
		v = -1;
	return c->invert ? -v : v;
}

void joy_stick_default(struct joy_stick_cal *c, float rest_x, float rest_y)
{
	int k;

	c->cx = rest_x;
	c->cy = rest_y;
	c->xn = fmaxf(1 + rest_x, MIN_REACH);
	c->xp = fmaxf(1 - rest_x, MIN_REACH);
	c->yn = fmaxf(1 + rest_y, MIN_REACH);
	c->yp = fmaxf(1 - rest_y, MIN_REACH);
	for (k = 0; k < JOY_SECTORS; k++)
		c->edge[k] = 1;
}

/* Offset from the rest position -> each half axis in units of its reach */
static void scale(const struct joy_stick_cal *c, float dx, float dy, float *ux, float *uy)
{
	*ux = dx / (dx < 0 ? c->xn : c->xp);
	*uy = dy / (dy < 0 ? c->yn : c->yp);
}

static float edge_at(const struct joy_stick_cal *c, float angle)
{
	float f = angle / SECTOR, t;
	int k;

	if (f < 0)
		f += JOY_SECTORS;
	k = (int)f;
	t = f - (float)k;
	k %= JOY_SECTORS;
	return c->edge[k] * (1 - t) + c->edge[(k + 1) % JOY_SECTORS] * t;
}

void joy_stick_apply(const struct joy_stick_cal *c, float deadzone, float x, float y,
		     float *out_x, float *out_y)
{
	float ux, uy, len, r;

	scale(c, x - c->cx, y - c->cy, &ux, &uy);
	len = hypotf(ux, uy);
	*out_x = *out_y = 0;
	if (len <= 0)
		return;
	r = len / edge_at(c, atan2f(uy, ux));
	if (r > 1)
		r = 1;
	if (r <= deadzone)
		return;
	/* keep the output continuous at the dead zone's edge */
	r = (r - deadzone) / (1 - deadzone);
	*out_x = ux / len * r;
	*out_y = uy / len * r;
}

int joy_stick_format(const struct joy_stick_cal *c, char *buf, size_t size)
{
	int n = snprintf(buf, size, "%.4f %.4f %.4f %.4f %.4f %.4f", (double)c->cx, (double)c->cy,
			 (double)c->xn, (double)c->xp, (double)c->yn, (double)c->yp);
	int k;

	for (k = 0; k < JOY_SECTORS && n >= 0 && (size_t)n < size; k++)
		n += snprintf(buf + n, size - (size_t)n, " %.4f", (double)c->edge[k]);
	return n >= 0 && (size_t)n < size ? n : -1;
}

bool joy_stick_parse(const char *s, struct joy_stick_cal *c)
{
	float v[6 + JOY_SECTORS];
	char *end;
	size_t i;

	for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		v[i] = strtof(s, &end);
		if (end == s || !isfinite(v[i]))
			return false;
		s = end;
	}
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	if (*s || fabsf(v[0]) > 0.5f || fabsf(v[1]) > 0.5f)
		return false;
	for (i = 2; i < 6; i++)
		if (v[i] < MIN_REACH || v[i] > 2)
			return false;
	for (; i < sizeof(v) / sizeof(v[0]); i++)
		if (v[i] < 0.3f || v[i] > 2)
			return false;
	c->cx = v[0];
	c->cy = v[1];
	c->xn = v[2];
	c->xp = v[3];
	c->yn = v[4];
	c->yp = v[5];
	memcpy(c->edge, v + 6, sizeof(c->edge));
	return true;
}

void joy_cal_begin(struct joy_cal_rec *r, float cx, float cy)
{
	memset(r, 0, sizeof(*r));
	r->cx = cx;
	r->cy = cy;
}

void joy_cal_add(struct joy_cal_rec *r, float x, float y)
{
	float dx = x - r->cx, dy = y - r->cy, len = hypotf(dx, dy);
	int bin;

	r->xmin = fminf(r->xmin, dx);
	r->xmax = fmaxf(r->xmax, dx);
	r->ymin = fminf(r->ymin, dy);
	r->ymax = fmaxf(r->ymax, dy);
	if (len < BIN_MIN)
		return;
	bin = (int)floorf((atan2f(dy, dx) + TWO_PI / 2) / TWO_PI * JOY_CAL_BINS);
	bin = (bin % JOY_CAL_BINS + JOY_CAL_BINS) % JOY_CAL_BINS;
	if (len > r->br[bin]) {
		r->bx[bin] = dx;
		r->by[bin] = dy;
		r->br[bin] = len;
	}
}

/* The reaches seen so far; false while a half axis hardly moved */
static bool rec_reach(const struct joy_cal_rec *r, struct joy_stick_cal *c)
{
	c->cx = r->cx;
	c->cy = r->cy;
	c->xn = -r->xmin;
	c->xp = r->xmax;
	c->yn = -r->ymin;
	c->yp = r->ymax;
	return c->xn >= MIN_REACH && c->xp >= MIN_REACH && c->yn >= MIN_REACH && c->yp >= MIN_REACH;
}

/* The kept points after scaling: angle and distance, those near the edge */
static int rec_points(const struct joy_cal_rec *r, const struct joy_stick_cal *c, float min,
		      float *angle, float *dist)
{
	int i, n = 0;

	for (i = 0; i < JOY_CAL_BINS; i++) {
		float ux, uy, len;

		if (r->br[i] <= 0)
			continue;
		scale(c, r->bx[i], r->by[i], &ux, &uy);
		len = hypotf(ux, uy);
		if (len < min)
			continue;
		angle[n] = atan2f(uy, ux);
		dist[n++] = len;
	}
	return n;
}

int joy_cal_covered(const struct joy_cal_rec *r)
{
	float angle[JOY_CAL_BINS], dist[JOY_CAL_BINS];
	struct joy_stick_cal c;
	unsigned seen = 0;
	int i, n, count = 0;

	if (!rec_reach(r, &c))
		return 0;
	n = rec_points(r, &c, EDGE_SEEN, angle, dist);
	for (i = 0; i < n; i++) {
		int k = (int)lrintf(angle[i] / SECTOR);

		seen |= 1u << ((k % JOY_SECTORS + JOY_SECTORS) % JOY_SECTORS);
	}
	for (i = 0; i < JOY_SECTORS; i++)
		count += (seen >> i) & 1;
	return count;
}

/* a - b in (-pi, pi] */
static float angle_diff(float a, float b)
{
	float d = fmodf(a - b, TWO_PI);

	if (d > TWO_PI / 2)
		d -= TWO_PI;
	else if (d <= -TWO_PI / 2)
		d += TWO_PI;
	return d;
}

bool joy_cal_finish(const struct joy_cal_rec *r, struct joy_stick_cal *out)
{
	float angle[JOY_CAL_BINS], dist[JOY_CAL_BINS];
	struct joy_stick_cal c;
	int i, k, n;

	if (!rec_reach(r, &c) || joy_cal_covered(r) < JOY_SECTORS)
		return false;
	n = rec_points(r, &c, EDGE_SEEN, angle, dist);
	/* the edge at each sector's direction: between the nearest points on
	 * both sides of it */
	for (k = 0; k < JOY_SECTORS; k++) {
		float at = (float)k * SECTOR, before = -TWO_PI, after = TWO_PI, rb = 0, ra = 0;

		for (i = 0; i < n; i++) {
			float d = angle_diff(angle[i], at);

			if (d <= 0 && d > before) {
				before = d;
				rb = dist[i];
			}
			if (d >= 0 && d < after) {
				after = d;
				ra = dist[i];
			}
		}
		if (rb <= 0 || ra <= 0 || after - before > EDGE_MAX_GAP)
			return false;
		c.edge[k] = (after > before ? rb + (ra - rb) * -before / (after - before) : rb) *
			    EDGE_MARGIN;
	}
	*out = c;
	return true;
}

int joy_direction(int prev, float v, float press, float release)
{
	if (prev > 0)
		return v > release ? 1 : (v < -press ? -1 : 0);
	if (prev < 0)
		return v < -release ? -1 : (v > press ? 1 : 0);
	return v > press ? 1 : (v < -press ? -1 : 0);
}
