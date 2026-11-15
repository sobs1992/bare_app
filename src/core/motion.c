#include "motion.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEG2RAD 0.017453292519943295f
#define REC_SPREAD_G 3.0f		/* deg/s: the calibration's "still" */
#define REC_SPREAD_A 0.05f		/* g */

static float dot(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float len3(const float v[3])
{
	return sqrtf(dot(v, v));
}

static bool normalize(float v[3])
{
	float l = len3(v);

	if (l < 1e-6f)
		return false;
	v[0] /= l;
	v[1] /= l;
	v[2] /= l;
	return true;
}

void motion_init(struct motion *m)
{
	memset(m, 0, sizeof(*m));
	motion_set_sensor(m, MOTION_CH_GYRO | MOTION_CH_ACCEL, 0, 0);
}

/* k times the noise within lo..hi, def if the noise is unknown */
static float by_noise(float rms, float k, float lo, float hi, float def)
{
	float t = rms * k;

	return !(rms > 0) ? def : t < lo ? lo : t > hi ? hi : t;
}

void motion_set_sensor(struct motion *m, unsigned channels, float gyro_rms, float accel_rms)
{
	m->channels = channels & (MOTION_CH_GYRO | MOTION_CH_ACCEL);
	m->still_gyro = by_noise(gyro_rms, MOTION_STILL_NOISE, MOTION_STILL_MIN, MOTION_STILL_MAX,
				 MOTION_STILL_GYRO);
	m->still_accel = by_noise(accel_rms, MOTION_STILL_NOISE, MOTION_STILL_ACCEL_MIN,
				  MOTION_STILL_ACCEL_MAX, MOTION_STILL_ACCEL);
	m->smooth_dps = by_noise(gyro_rms, MOTION_SMOOTH_NOISE, MOTION_SMOOTH_MIN, MOTION_SMOOTH_MAX_DPS,
				 MOTION_SMOOTH_DPS);
	m->tight_dps = by_noise(gyro_rms, MOTION_TIGHT_NOISE, MOTION_TIGHT_MIN, MOTION_TIGHT_MAX,
				MOTION_TIGHT_DPS);
	if (!(m->channels & MOTION_CH_GYRO)) {
		/* the gyro made from gravity has no zero to measure */
		memset(m->bias, 0, sizeof(m->bias));
		m->bias_known = true;
	}
}

void motion_can_aim(const struct motion *m, bool *x, bool *y)
{
	bool accel = m->channels & MOTION_CH_ACCEL;

	/* left and right: about gravity, or the calibrated vertical; without
	 * a gyro the turn about gravity cannot be seen */
	*x = m->channels & MOTION_CH_GYRO ? accel || m->cal.up_set : false;
	*y = m->channels && m->cal.axes_set;
}

float motion_synth_setup(struct motion_synth *s, float accel_rms, float rate_hz)
{
	/* the noise as at MOTION_SYNTH_REF_HZ: fewer samples, each counts more */
	float n = accel_rms * (rate_hz > 0 ? sqrtf(MOTION_SYNTH_REF_HZ / rate_hz) : 1);
	float span = n > 0 ? n * MOTION_SYNTH_PER_G : MOTION_SYNTH_MAX_S;

	memset(s, 0, sizeof(*s));
	s->span = span < MOTION_SYNTH_MIN_S ? MOTION_SYNTH_MIN_S : span > MOTION_SYNTH_MAX_S ? MOTION_SYNTH_MAX_S : span;
	/* the turn, sample to sample, of the 'span' mean of an "up" smoothed
	 * over 'span': about 3.85 n / span^1.5 deg/s (measured on a real
	 * accelerometer at rest, 1139 Hz: 0.29 deg/s for 0.0022 g, span 0.1 s) */
	return n > 0 ? 3.85f * n / powf(s->span, 1.5f) : 0;
}

void motion_synth_reset(struct motion_synth *s)
{
	s->set = false;
	s->have_v = false;
	s->count = 0;
	s->head = 0;
}

void motion_synth_gyro(struct motion_synth *s, const float accel[3], float dt, float gyro[3])
{
	float a[3] = { accel[0], accel[1], accel[2] }, v[3] = { 0, 0, 0 }, held = 0, k;
	unsigned i;

	gyro[0] = gyro[1] = gyro[2] = 0;
	if (!normalize(a) || !(dt > 0))
		return;
	if (!s->set) {
		memcpy(s->up, a, sizeof(s->up));
		s->set = true;
		s->count = 0;
		s->have_v = false;
	}
	if (!(s->span > 0))
		s->span = MOTION_SYNTH_MAX_S;
	k = dt / (s->span + dt);
	for (i = 0; i < 3; i++)
		s->up[i] += (a[i] - s->up[i]) * k;
	normalize(s->up);
	/* the smoothed "up" kept with the time it stands for */
	memcpy(s->hist[s->head], s->up, sizeof(s->up));
	s->hdt[s->head] = dt;
	s->head = (s->head + 1) % MOTION_SYNTH_MAX;
	if (s->count < MOTION_SYNTH_MAX)
		s->count++;
	/* its mean over the last 'span' seconds, each by the time it stood */
	for (i = 1; i <= s->count && held < s->span; i++) {
		unsigned j = (s->head + MOTION_SYNTH_MAX - i) % MOTION_SYNTH_MAX;
		float w = fminf(s->hdt[j], s->span - held);

		for (int c = 0; c < 3; c++)
			v[c] += s->hist[j][c] * w;
		held += w;
	}
	if (!normalize(v))
		return;
	if (s->have_v) {
		/* the turn of that mean since the sample before: these add up to
		 * the whole turn whatever the steps. A vector fixed in the world
		 * turns by -w x v as seen from the sensor: w is along new x old,
		 * by the angle between them (atan2: right for a big step too) */
		float c[3] = { v[1] * s->v_prev[2] - v[2] * s->v_prev[1],
			       v[2] * s->v_prev[0] - v[0] * s->v_prev[2],
			       v[0] * s->v_prev[1] - v[1] * s->v_prev[0] };
		float sn = len3(c), angle = atan2f(sn, dot(v, s->v_prev));

		if (sn > 1e-9f)
			for (int i2 = 0; i2 < 3; i2++)
				gyro[i2] = c[i2] / sn * angle / dt / DEG2RAD;
	}
	memcpy(s->v_prev, v, sizeof(v));
	s->have_v = true;
}

void motion_set_cal(struct motion *m, const struct motion_cal *c)
{
	if (c)
		m->cal = *c;
	else
		memset(&m->cal, 0, sizeof(m->cal));
	/* the gyro made from gravity (no gyro) has no zero: none is taken */
	if (c && c->bias_set && (m->channels & MOTION_CH_GYRO)) {
		memcpy(m->bias, c->bias, sizeof(m->bias));
		m->bias_known = true;
	}
	if (m->cal.axes_set && !normalize(m->cal.right))
		m->cal.axes_set = false;
	if (m->cal.up_set && !normalize(m->cal.up))
		m->cal.up_set = false;
}

static void still_reset(struct motion *m)
{
	memset(m->still_g, 0, sizeof(m->still_g));
	memset(m->still_g2, 0, sizeof(m->still_g2));
	memset(m->still_a, 0, sizeof(m->still_a));
	memset(m->still_a2, 0, sizeof(m->still_a2));
	m->still_t = 0;
	m->still_n = 0;
}

/* A window of samples moving less than the noise: the gyro's zero */
static void still_add(struct motion *m, const float gyro[3], const float accel[3], float dt)
{
	bool still = true;
	double mean[3];
	int k;

	for (k = 0; k < 3; k++) {
		m->still_g[k] += gyro[k];
		m->still_g2[k] += (double)gyro[k] * gyro[k];
		m->still_a[k] += accel[k];
		m->still_a2[k] += (double)accel[k] * accel[k];
	}
	m->still_n++;
	m->still_t += dt;
	if (m->still_t * 1000 < MOTION_STILL_MS)
		return;
	for (k = 0; k < 3; k++) {
		double n = m->still_n, mg = m->still_g[k] / n, ma = m->still_a[k] / n;
		double vg = m->still_g2[k] / n - mg * mg, va = m->still_a2[k] / n - ma * ma;

		mean[k] = mg;
		if (vg > (double)m->still_gyro * m->still_gyro ||
		    ((m->channels & MOTION_CH_ACCEL) &&
		     va > (double)m->still_accel * m->still_accel) || fabs(mg) > MOTION_BIAS_MAX ||
		    /* a slow steady turn is not a new zero */
		    (m->bias_known && fabs(mg - m->bias[k]) > MOTION_BIAS_STEP))
			still = false;
	}
	m->still = still;
	if (still && (m->channels & MOTION_CH_GYRO)) {
		for (k = 0; k < 3; k++)
			m->bias[k] = (float)mean[k];
		m->bias_known = true;
	}
	still_reset(m);
}

/* "Up" turned with the device, pulled towards the accelerometer */
static void gravity(struct motion *m, const float g[3], const float accel[3], float dt)
{
	float a[3] = { accel[0], accel[1], accel[2] }, w[3], c[3], an = len3(a), k;
	int i;

	if (an < 0.3f || an > 3.0f)	/* falling, or a knock */
		return;
	normalize(a);
	if (!m->up_set) {
		memcpy(m->up, a, sizeof(m->up));
		m->up_set = true;
		return;
	}
	/* a vector fixed in the world, seen from the sensor: dv/dt = -w x v */
	for (i = 0; i < 3; i++)
		w[i] = g[i] * DEG2RAD;
	c[0] = w[1] * m->up[2] - w[2] * m->up[1];
	c[1] = w[2] * m->up[0] - w[0] * m->up[2];
	c[2] = w[0] * m->up[1] - w[1] * m->up[0];
	k = dt / (MOTION_GRAVITY_S + dt);
	for (i = 0; i < 3; i++)
		m->up[i] += -c[i] * dt + (a[i] - m->up[i]) * k;
	if (!normalize(m->up))
		memcpy(m->up, a, sizeof(m->up));
}

/* Turns slower than m->smooth_dps go through an average of the last
 * MOTION_SMOOTH_S, faster ones as they are, in between some of each */
static void smooth(struct motion *m, float *rx, float *ry, float dt)
{
	float mag = hypotf(*rx, *ry), half = m->smooth_dps / 2, w, sx = 0, sy = 0, st = 0;
	unsigned i, idx;

	if (mag < m->tight_dps) {
		*rx *= mag / m->tight_dps;
		*ry *= mag / m->tight_dps;
	}
	w = (mag - half) / half;
	w = w < 0 ? 0 : w > 1 ? 1 : w;
	idx = m->sm_head;
	m->sm_x[idx] = *rx * (1 - w);
	m->sm_y[idx] = *ry * (1 - w);
	m->sm_dt[idx] = dt;
	m->sm_head = (idx + 1) % MOTION_SMOOTH_MAX;
	if (m->sm_count < MOTION_SMOOTH_MAX)
		m->sm_count++;
	for (i = 0; i < m->sm_count && st < MOTION_SMOOTH_S; i++) {
		unsigned j = (idx + MOTION_SMOOTH_MAX - i) % MOTION_SMOOTH_MAX;

		sx += m->sm_x[j] * m->sm_dt[j];
		sy += m->sm_y[j] * m->sm_dt[j];
		st += m->sm_dt[j];
	}
	*rx = *rx * w + (st > 0 ? sx / st : 0);
	*ry = *ry * w + (st > 0 ? sy / st : 0);
}

void motion_feed(struct motion *m, const float gyro[3], const float accel[3], float dt)
{
	float g[3], yaw, pitch = 0, rx, ry;
	int k;

	if (!(dt > 0) || dt > MOTION_DT_MAX)
		dt = dt > MOTION_DT_MAX ? MOTION_DT_MAX : 0;
	still_add(m, gyro, accel, dt);
	for (k = 0; k < 3; k++)
		g[k] = gyro[k] - m->bias[k];
	if (m->channels & MOTION_CH_ACCEL) {
		gravity(m, g, accel, dt);
		if (!m->up_set)
			return;
	}
	if (!dt)
		return;
	if (!(m->channels & MOTION_CH_ACCEL)) {
		/* no gravity: the device's own axes, as calibrated */
		yaw = m->cal.up_set ? dot(g, m->cal.up) : 0;
		pitch = m->cal.axes_set ? dot(g, m->cal.right) : 0;
	} else if (!m->cal.axes_set) {
		yaw = dot(g, m->up);
	} else {
		float perp[3], wy, pl;

		pitch = dot(g, m->cal.right);
		for (k = 0; k < 3; k++)
			perp[k] = g[k] - pitch * m->cal.right[k];
		/* the turn less the tilt, about "up": a device held tilted that
		 * turns about its own vertical turns the view as much */
		wy = dot(perp, m->up);
		pl = len3(perp);
		yaw = fabsf(wy) * MOTION_YAW_RELAX < pl ? wy * MOTION_YAW_RELAX : copysignf(pl, wy);
	}
	/* left about "up" is positive: the view goes left, x the other way;
	 * the top tilted towards the player is positive about the right: the
	 * view goes down, as the user wanted it (a camera would look up; the
	 * sensor's tune can invert it) */
	rx = -yaw;
	ry = pitch;
	m->rate_x = rx;
	m->rate_y = ry;
	if (!m->bias_known)
		return;
	smooth(m, &rx, &ry, dt);
	m->x += rx * dt;
	m->y += ry * dt;
}

void motion_take(struct motion *m, float *x, float *y)
{
	*x = m->x;
	*y = m->y;
	m->x = m->y = 0;
}

void motion_drop(struct motion *m)
{
	m->x = m->y = 0;
	m->sm_count = 0;
}

/* ---- calibration -------------------------------------------------------- */

void motion_rec_begin(struct motion_rec *r)
{
	memset(r, 0, sizeof(*r));
}

bool motion_rec_still(struct motion_rec *r, const float gyro[3], const float accel[3])
{
	int k;

	for (k = 0; k < 3; k++) {
		if (!r->n || gyro[k] < r->gmin[k])
			r->gmin[k] = gyro[k];
		if (!r->n || gyro[k] > r->gmax[k])
			r->gmax[k] = gyro[k];
		if (!r->n || accel[k] < r->amin[k])
			r->amin[k] = accel[k];
		if (!r->n || accel[k] > r->amax[k])
			r->amax[k] = accel[k];
		r->g[k] += gyro[k];
	}
	r->n++;
	r->spread_g = r->spread_a = 0;
	for (k = 0; k < 3; k++) {
		r->spread_g = fmaxf(r->spread_g, r->gmax[k] - r->gmin[k]);
		r->spread_a = fmaxf(r->spread_a, r->amax[k] - r->amin[k]);
	}
	return r->spread_g < REC_SPREAD_G && r->spread_a < REC_SPREAD_A;
}

bool motion_rec_bias(const struct motion_rec *r, float bias[3])
{
	int k;

	if (!r->n)
		return false;
	for (k = 0; k < 3; k++)
		bias[k] = (float)(r->g[k] / r->n);
	return true;
}

void motion_rec_tilt_begin(struct motion_rec *r, const float up[3])
{
	memset(r->turn, 0, sizeof(r->turn));
	memcpy(r->up, up, sizeof(r->up));
}

static void tilt_perp(const struct motion_rec *r, float perp[3])
{
	float t[3] = { (float)r->turn[0], (float)r->turn[1], (float)r->turn[2] };
	float u = dot(t, r->up);
	int k;

	for (k = 0; k < 3; k++)
		perp[k] = t[k] - u * r->up[k];
}

float motion_rec_tilt(struct motion_rec *r, const float gyro[3], const float bias[3], float dt)
{
	float perp[3];
	int k;

	for (k = 0; k < 3; k++)
		r->turn[k] += (double)(gyro[k] - bias[k]) * dt;
	tilt_perp(r, perp);
	return len3(perp);
}

bool motion_rec_right(const struct motion_rec *r, float right[3])
{
	float t[3] = { (float)r->turn[0], (float)r->turn[1], (float)r->turn[2] }, perp[3];

	tilt_perp(r, perp);
	if (len3(perp) < 0.7f * len3(t) || !normalize(perp))
		return false;
	memcpy(right, perp, sizeof(perp));
	return true;
}

int motion_cal_format(const struct motion_cal *c, char *buf, size_t size)
{
	int n = 0, w;

	if (size)
		buf[0] = '\0';
	if (c->bias_set) {
		w = snprintf(buf, size, "bias %.4f %.4f %.4f", (double)c->bias[0], (double)c->bias[1],
			     (double)c->bias[2]);
		if (w < 0 || (size_t)w >= size)
			return -1;
		n = w;
	}
	if (c->axes_set) {
		w = snprintf(buf + n, size - n, "%sright %.4f %.4f %.4f", n ? " " : "",
			     (double)c->right[0], (double)c->right[1], (double)c->right[2]);
		if (w < 0 || (size_t)w >= size - n)
			return -1;
		n += w;
	}
	if (c->up_set) {
		w = snprintf(buf + n, size - n, "%sup %.4f %.4f %.4f", n ? " " : "",
			     (double)c->up[0], (double)c->up[1], (double)c->up[2]);
		if (w < 0 || (size_t)w >= size - n)
			return -1;
		n += w;
	}
	return n;
}

static bool parse3(const char **s, float v[3])
{
	char *end;
	int k;

	for (k = 0; k < 3; k++) {
		v[k] = strtof(*s, &end);
		if (end == *s || !isfinite(v[k]))
			return false;
		*s = end;
	}
	return true;
}

bool motion_cal_parse(const char *s, struct motion_cal *c)
{
	struct motion_cal r = { 0 };

	for (;;) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s || *s == '\n' || *s == '\r')
			break;
		if (!strncmp(s, "bias", 4)) {
			s += 4;
			if (!parse3(&s, r.bias))
				return false;
			r.bias_set = true;
		} else if (!strncmp(s, "right", 5)) {
			s += 5;
			if (!parse3(&s, r.right) || !normalize(r.right))
				return false;
			r.axes_set = true;
		} else if (!strncmp(s, "up", 2)) {
			s += 2;
			if (!parse3(&s, r.up) || !normalize(r.up))
				return false;
			r.up_set = true;
		} else {
			return false;
		}
	}
	*c = r;
	return true;
}
