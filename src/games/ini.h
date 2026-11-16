/*
 * The ini files of the games (wine.ini, game.ini): "key = value" lines,
 * "[section]" headers, comments from ';' or '#' at the start of a line or
 * after a blank. Keys and sections as written; values trimmed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* cb gets every value with its section ("" before the first); false from
 * cb stops. -1 if the file cannot be read (errno), else 0. */
int ini_read(const char *path,
	     bool (*cb)(const char *section, const char *key, const char *value, void *ud), void *ud);

/* A value with "\n" for new lines (hints) into dst, cut to size */
void ini_copy_lines(char *dst, size_t size, const char *src);

/* "yes", "true" or "1" (any case) */
bool ini_yes(const char *value);
