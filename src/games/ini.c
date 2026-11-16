#include "ini.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static char *trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

int ini_read(const char *path,
	     bool (*cb)(const char *section, const char *key, const char *value, void *ud), void *ud)
{
	char line[1024], section[64] = "", *key, *eq, *value, *end;
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		key = trim(line);
		if (*key == ';' || *key == '#' || !*key)
			continue;
		if (*key == '[') {
			end = strchr(key, ']');
			if (end)
				*end = '\0';
			snprintf(section, sizeof(section), "%s", trim(key + 1));
			continue;
		}
		eq = strchr(key, '=');
		if (!eq)
			continue;
		*eq = '\0';
		value = trim(eq + 1);
		/* a comment after the value */
		for (char *c = value; *c; c++)
			if ((*c == ';' || *c == '#') && c > value && isspace((unsigned char)c[-1])) {
				*c = '\0';
				value = trim(value);
				break;
			}
		if (!cb(section, trim(key), value, ud))
			break;
	}
	fclose(f);
	return 0;
}

bool ini_yes(const char *value)
{
	return !strcasecmp(value, "yes") || !strcasecmp(value, "true") || !strcmp(value, "1");
}

void ini_copy_lines(char *dst, size_t size, const char *src)
{
	size_t n = 0;

	if (!size)
		return;
	for (; *src && n + 1 < size; src++)
		if (src[0] == '\\' && src[1] == 'n') {
			dst[n++] = '\n';
			src++;
		} else {
			dst[n++] = *src;
		}
	dst[n] = '\0';
}
