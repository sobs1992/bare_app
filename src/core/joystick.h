/* Analog stick math, no I/O: easy to test on the host. */
#pragma once

#include <stdbool.h>

struct joy_axis_cal {
	int min, max;		/* raw range reported by the driver */
	int center;		/* raw value at rest */
	float deadzone;		/* 0..1 of the half range around center */
	bool invert;
};

/* Raw value -> -1..1, 0 inside the dead zone, rescaled outside it */
float joy_normalize(const struct joy_axis_cal *c, int raw);

/*
 * Direction with hysteresis: prev is -1/0/+1, returns the new one.
 * A direction turns on beyond 'press' and off below 'release'.
 */
int joy_direction(int prev, float v, float press, float release);
