#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/log.h"
#include "core/mainloop.h"

#define TAG "config"
#define MAX_KEYS 32
#define SAVE_DELAY_MS 1000

static struct {
	struct mainloop *ml;
	char path[256];
	struct { char key[32], value[64]; } kv[MAX_KEYS];
	int n;
	struct ml_timer *save;
} cf;

static int find(const char *key)
{
	int i;

	for (i = 0; i < cf.n; i++)
		if (!strcmp(cf.kv[i].key, key))
			return i;
	return -1;
}

static void put(const char *key, const char *value)
{
	int i = find(key);

	if (i < 0) {
		if (cf.n == MAX_KEYS)
			return;
		i = cf.n++;
		snprintf(cf.kv[i].key, sizeof(cf.kv[i].key), "%s", key);
	}
	snprintf(cf.kv[i].value, sizeof(cf.kv[i].value), "%s", value);
}

static void load(void)
{
	FILE *f = fopen(cf.path, "r");
	char line[128];

	if (!f) {
		if (errno != ENOENT)
			LOGW(TAG, "%s: %s", cf.path, strerror(errno));
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');

		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#' || !eq)
			continue;
		*eq = '\0';
		put(line, eq + 1);
	}
	fclose(f);
	LOGI(TAG, "%s: %d settings", cf.path, cf.n);
}

static void save(void *ud)
{
	char tmp[sizeof(cf.path) + 8], dir[sizeof(cf.path)], *slash;
	FILE *f;
	int i;

	(void)ud;
	cf.save = NULL;
	if (!*cf.path)
		return;
	snprintf(dir, sizeof(dir), "%s", cf.path);
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = '\0';
		mkdir(dir, 0755);	/* ~/.config may not exist yet */
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", cf.path);
	f = fopen(tmp, "w");
	if (!f) {
		LOGW(TAG, "%s: %s", tmp, strerror(errno));
		return;
	}
	fprintf(f, "# bare_app settings, rewritten by the application\n");
	for (i = 0; i < cf.n; i++)
		fprintf(f, "%s=%s\n", cf.kv[i].key, cf.kv[i].value);
	if (fflush(f) || fsync(fileno(f)) || fclose(f)) {
		LOGW(TAG, "%s: %s", tmp, strerror(errno));
		unlink(tmp);
		return;
	}
	if (rename(tmp, cf.path) < 0)
		LOGW(TAG, "%s: %s", cf.path, strerror(errno));
	else
		LOGD(TAG, "saved %s", cf.path);
}

int config_open(struct mainloop *ml, const char *path)
{
	const char *home = getenv("HOME");

	memset(&cf, 0, sizeof(cf));
	cf.ml = ml;
	if (!path)
		return 0;
	if (!strncmp(path, "~/", 2)) {
		if (!home)
			return 0;
		snprintf(cf.path, sizeof(cf.path), "%s/%s", home, path + 2);
	} else {
		snprintf(cf.path, sizeof(cf.path), "%s", path);
	}
	load();
	return 0;
}

void config_close(void)
{
	if (cf.save) {
		mainloop_timer_cancel(cf.ml, cf.save);
		save(NULL);
	}
}

const char *config_get(const char *key)
{
	int i = find(key);

	return i < 0 ? NULL : cf.kv[i].value;
}

int config_get_int(const char *key, int def)
{
	const char *v = config_get(key);
	char *end;
	long n;

	if (!v)
		return def;
	n = strtol(v, &end, 10);
	return end == v ? def : (int)n;
}

void config_set(const char *key, const char *value)
{
	const char *old = config_get(key);

	if (old && !strcmp(old, value))
		return;
	put(key, value);
	if (*cf.path && !cf.save)
		cf.save = mainloop_add_timer(cf.ml, SAVE_DELAY_MS, 0, save, NULL);
}

void config_set_int(const char *key, int value)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%d", value);
	config_set(key, buf);
}
