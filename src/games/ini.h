/*
 * The ini files of the games (wine.ini, game.ini): "key = value" lines,
 * "[section]" headers, comments from ';' or '#' at the start of a line or
 * after a blank. Keys and sections as written; values trimmed.
 */
#pragma once

#include <stdbool.h>

/* cb gets every value with its section ("" before the first); false from
 * cb stops. -1 if the file cannot be read (errno), else 0. */
int ini_read(const char *path,
	     bool (*cb)(const char *section, const char *key, const char *value, void *ud), void *ud);
