#include "joystick.h"

float joy_normalize(const struct joy_axis_cal *c, int raw)
{
	float v, half, mag;

	if (raw >= c->center)
		half = (float)(c->max - c->center);
	else
		half = (float)(c->center - c->min);
	if (half <= 0)
		return 0;
	v = (float)(raw - c->center) / half;
	if (v > 1)
		v = 1;
	if (v < -1)
		v = -1;

	mag = v < 0 ? -v : v;
	if (mag <= c->deadzone)
		return 0;
	/* keep the output continuous at the dead zone edge */
	mag = (mag - c->deadzone) / (1 - c->deadzone);
	v = v < 0 ? -mag : mag;
	return c->invert ? -v : v;
}

int joy_direction(int prev, float v, float press, float release)
{
	if (prev > 0)
		return v > release ? 1 : (v < -press ? -1 : 0);
	if (prev < 0)
		return v < -release ? -1 : (v > press ? 1 : 0);
	return v > press ? 1 : (v < -press ? -1 : 0);
}
