#include "platform.h"

#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "core/util.h"

extern const struct platform platform_oga, platform_ogu, platform_pc, platform_pc_mem;

static const struct platform *const platforms[] = {
#ifdef CONFIG_PLATFORM_OGA
	&platform_oga,
#endif
#ifdef CONFIG_PLATFORM_OGU
	&platform_ogu,
#endif
#ifdef CONFIG_PLATFORM_PC
	&platform_pc,
	&platform_pc_mem,
#endif
};

static char model[64];

/* /proc/device-tree/compatible is a list of NUL-terminated strings */
static const struct platform *probe_dt(void)
{
	char buf[512];
	FILE *f = fopen("/proc/device-tree/compatible", "r");
	size_t n, i, j;
	const char *s;

	if (!f)
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	for (s = buf; s < buf + n; s += strlen(s) + 1)
		for (i = 0; i < ARRAY_SIZE(platforms); i++)
			for (j = 0; platforms[i]->compatible && platforms[i]->compatible[j]; j++)
				if (!strcmp(s, platforms[i]->compatible[j])) {
					LOGI("platform", "matched \"%s\"", s);
					return platforms[i];
				}
	return NULL;
}

const struct platform *platform_probe(const char *name)
{
	const struct platform *p = NULL;
	size_t i;

	read_text_file("/proc/device-tree/model", model, sizeof(model));
	if (name) {
		for (i = 0; i < ARRAY_SIZE(platforms); i++)
			if (!strcmp(platforms[i]->name, name))
				p = platforms[i];
		if (!p)
			LOGE("platform", "unknown platform \"%s\"", name);
		return p;
	}
	p = probe_dt();
#ifdef CONFIG_PLATFORM_PC
	if (!p)
		p = &platform_pc;
#endif
	if (!p)
		LOGE("platform", "this board is not supported by this build (see -l)");
	return p;
}

void platform_list(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(platforms); i++)
		printf("%s\n", platforms[i]->name);
}

const char *platform_model(void)
{
	return model;
}
