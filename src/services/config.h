/*
 * Persistent settings (control thread): a small "key=value" file. Writes
 * are delayed by a second so a dragged slider does not rewrite the file
 * on every step, and replace the file atomically (temporary + rename).
 * Without a path (tests) everything stays in memory.
 */
#pragma once

struct mainloop;

int config_open(struct mainloop *ml, const char *path);	/* "~/" means $HOME */
void config_close(void);	/* writes pending changes */

const char *config_get(const char *key);		/* NULL if unset */
int config_get_int(const char *key, int def);
void config_set(const char *key, const char *value);
void config_set_int(const char *key, int value);
void config_unset(const char *key);
/* Every key starting with prefix, in no order */
void config_each(const char *prefix, void (*cb)(const char *key, const char *value, void *ud),
		 void *ud);
