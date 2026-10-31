#include "sysfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"

#define TAG "sysfs"

static int read_attr(const char *dir, const char *attr, char *buf, int size)
{
	char path[160];

	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	return read_text_file(path, buf, size);
}

static int read_attr_int(const char *dir, const char *attr, int *val)
{
	char path[160];

	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	return read_int_file(path, val);
}

/* ---- power supply ----------------------------------------------------- */

static void *power_open(const void *params)
{
	const struct power_sysfs_params *p = params;

	if (access(p->battery, R_OK) < 0) {
		LOGW(TAG, "no battery at %s", p->battery);
		return NULL;
	}
	return (void *)p;
}

static int power_read(void *h, struct hal_power_state *st)
{
	const struct power_sysfs_params *p = h;
	char status[32] = "";
	int v;

	memset(st, 0, sizeof(*st));
	st->capacity = -1;
	if (read_attr_int(p->battery, "present", &v) == 0)
		st->present = v;
	else
		st->present = true;
	if (read_attr_int(p->battery, "capacity", &v) == 0)
		st->capacity = CLAMP(v, 0, 100);
	read_attr(p->battery, "status", status, sizeof(status));
	st->charging = !strcmp(status, "Charging");
	st->full = !strcmp(status, "Full");
	if (p->charger && read_attr_int(p->charger, "online", &v) == 0)
		st->online = v;
	else
		st->online = st->charging || st->full;
	return 0;
}

static void power_close(void *h)
{
	(void)h;
}

const struct hal_power_ops power_sysfs = {
	.name = "power-sysfs",
	.open = power_open,
	.read = power_read,
	.close = power_close,
};

/* ---- backlight -------------------------------------------------------- */

struct backlight {
	const struct backlight_sysfs_params *p;
	int max;
};

static void *backlight_open(const void *params)
{
	const struct backlight_sysfs_params *p = params;
	struct backlight *b;
	int max;

	if (read_attr_int(p->path, "max_brightness", &max) < 0 || max <= 0) {
		LOGW(TAG, "no backlight at %s", p->path);
		return NULL;
	}
	b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;
	b->p = p;
	b->max = max;
	return b;
}

static int backlight_get(void *h)
{
	struct backlight *b = h;
	int v;

	if (read_attr_int(b->p->path, "brightness", &v) < 0)
		return -1;
	return (v * 100 + b->max / 2) / b->max;
}

static int backlight_set(void *h, int percent)
{
	struct backlight *b = h;
	char path[160];

	percent = CLAMP(percent, b->p->min_percent, 100);
	snprintf(path, sizeof(path), "%s/brightness", b->p->path);
	return write_int_file(path, (percent * b->max + 50) / 100);
}

static void backlight_close(void *h)
{
	free(h);
}

const struct hal_backlight_ops backlight_sysfs = {
	.name = "backlight-sysfs",
	.open = backlight_open,
	.get = backlight_get,
	.set = backlight_set,
	.close = backlight_close,
};
