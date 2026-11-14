#include "remap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INVERT "invert"

static const char *const axis_names[GP_AXIS_COUNT] = { "lx", "ly", "rx", "ry" };

const char *gp_axis_name(enum gp_axis a)
{
	return a >= 0 && a < GP_AXIS_COUNT ? axis_names[a] : NULL;
}

static int axis_from_name(const char *name)
{
	int a;

	for (a = 0; a < GP_AXIS_COUNT; a++)
		if (!strcmp(name, axis_names[a]))
			return a;
	return -1;
}

void gp_remap_reset(struct gp_remap *m)
{
	int b;

	for (b = 0; b < GP_COUNT; b++)
		m->to[b] = (int8_t)b;
	m->invert = 0;
	m->gyro = GP_GYRO_GAME;
	m->gyro_sens = GP_GYRO_SENS_DEFAULT;
}

enum gp_button gp_remap_stick_dir(const struct gp_remap *m, enum gp_button b)
{
	switch (b) {
	case GP_LEFT:  return m->invert & GP_BIT(GP_AXIS_LX) ? GP_RIGHT : b;
	case GP_RIGHT: return m->invert & GP_BIT(GP_AXIS_LX) ? GP_LEFT : b;
	case GP_UP:    return m->invert & GP_BIT(GP_AXIS_LY) ? GP_DOWN : b;
	case GP_DOWN:  return m->invert & GP_BIT(GP_AXIS_LY) ? GP_UP : b;
	default:       return b;
	}
}

bool gp_remap_is_default(const struct gp_remap *m)
{
	int b;

	if (m->invert || m->gyro != GP_GYRO_GAME || m->gyro_sens != GP_GYRO_SENS_DEFAULT)
		return false;
	for (b = 0; b < GP_COUNT; b++)
		if (m->to[b] != b)
			return false;
	return true;
}

int gp_remap_parse(struct gp_remap *m, const char *text)
{
	char name[16], to[16];
	int ret = 0;

	gp_remap_reset(m);
	while (text && *text) {
		size_t len = strcspn(text, ","), eq = strcspn(text, "=");
		enum gp_button from, b = GP_NONE;
		int a;

		if (eq < len && eq < sizeof(name) && len - eq - 1 < sizeof(to)) {
			memcpy(name, text, eq);
			name[eq] = '\0';
			memcpy(to, text + eq + 1, len - eq - 1);
			to[len - eq - 1] = '\0';
			if (!strcmp(name, "gyro")) {
				if (!strcmp(to, "on") || !strcmp(to, "off"))
					m->gyro = !strcmp(to, "on");
				else
					ret = -1;
				text += len + (text[len] == ',');
				continue;
			}
			if (!strcmp(name, "gyrosens")) {
				char *end;
				long t = lrint(strtod(to, &end) * 10);

				if (end != to && !*end && t >= GP_GYRO_SENS_MIN && t <= GP_GYRO_SENS_MAX)
					m->gyro_sens = (uint8_t)t;
				else
					ret = -1;
				text += len + (text[len] == ',');
				continue;
			}
			if ((a = axis_from_name(name)) >= 0) {
				if (!strcmp(to, INVERT))
					m->invert |= (uint8_t)GP_BIT(a);
				else
					ret = -1;
				text += len + (text[len] == ',');
				continue;
			}
			from = gp_button_from_name(name);
			if (strcmp(to, "-"))
				b = gp_button_from_name(to);
			if (from != GP_NONE && (b != GP_NONE || !strcmp(to, "-")))
				m->to[from] = (int8_t)b;
			else
				ret = -1;
		} else if (len) {
			ret = -1;
		}
		text += len + (text[len] == ',');
	}
	return ret;
}

bool gp_remap_format(const struct gp_remap *m, char *buf, size_t size)
{
	size_t n = 0;
	int b, w;

	if (!size)
		return false;
	buf[0] = '\0';
	for (b = 0; b < GP_COUNT; b++) {
		if (m->to[b] == b)
			continue;
		w = snprintf(buf + n, size - n, "%s%s=%s", n ? "," : "", gp_button_name(b),
			     m->to[b] == GP_NONE ? "-" : gp_button_name(m->to[b]));
		if (w < 0 || (size_t)w >= size - n) {
			buf[n] = '\0';
			return false;
		}
		n += (size_t)w;
	}
	for (b = 0; b < GP_AXIS_COUNT; b++) {
		if (!(m->invert & GP_BIT(b)))
			continue;
		w = snprintf(buf + n, size - n, "%s%s=" INVERT, n ? "," : "", axis_names[b]);
		if (w < 0 || (size_t)w >= size - n) {
			buf[n] = '\0';
			return false;
		}
		n += (size_t)w;
	}
	if (m->gyro != GP_GYRO_GAME) {
		w = snprintf(buf + n, size - n, "%sgyro=%s", n ? "," : "", m->gyro ? "on" : "off");
		if (w < 0 || (size_t)w >= size - n) {
			buf[n] = '\0';
			return false;
		}
		n += (size_t)w;
	}
	if (m->gyro_sens != GP_GYRO_SENS_DEFAULT) {
		w = snprintf(buf + n, size - n, "%sgyrosens=%d.%d", n ? "," : "", m->gyro_sens / 10,
			     m->gyro_sens % 10);
		if (w < 0 || (size_t)w >= size - n) {
			buf[n] = '\0';
			return false;
		}
		n += (size_t)w;
	}
	return true;
}
