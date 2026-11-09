#include "remap.h"

#include <stdio.h>
#include <string.h>

void gp_remap_reset(struct gp_remap *m)
{
	int b;

	for (b = 0; b < GP_COUNT; b++)
		m->to[b] = (int8_t)b;
}

bool gp_remap_is_default(const struct gp_remap *m)
{
	int b;

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

		if (eq < len && eq < sizeof(name) && len - eq - 1 < sizeof(to)) {
			memcpy(name, text, eq);
			name[eq] = '\0';
			memcpy(to, text + eq + 1, len - eq - 1);
			to[len - eq - 1] = '\0';
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
	return true;
}
