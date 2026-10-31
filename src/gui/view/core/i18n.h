/*
 * UI strings (view side, toolkit independent). The language is chosen at
 * start; a string missing in a language falls back to English.
 */
#pragma once

enum str_id {
	S_INPUT_TEST, S_SETTINGS, S_LAYERS, S_QUIT,
	S_BRIGHTNESS, S_NOT_AVAILABLE, S_BATTERY, S_CHANGE_BACK,
	S_UNKNOWN, S_NONE, S_FULL, S_CHARGING, S_DISCHARGING, S_CHARGER_ON,
	S_TWICE_BACK,
	S_LAYERS_VIDEO, S_LAYERS_NO_VIDEO,
	S_BATTERY_LOW_FMT,	/* "%d" */
	S_CHARGER_CONNECTED, S_CHARGER_DISCONNECTED,
	S_LANGUAGE,
	S_LANG_NAME,		/* name of the current language in itself */
	S_DOOM, S_DOOM_HINT,
	S_DOOM_NO_WAD_FMT,	/* "%s": IWAD path */
	S_DOOM_FAILED,
	S_DOOM_EXIT_FMT,	/* "%d": exit status */
	S_DOOM_SIGNAL_FMT,	/* "%d": signal */
	S_COUNT
};

/* "en", "ru"; returns -1 (and keeps English) for an unknown language */
int i18n_set(const char *lang);
const char *i18n_lang(void);
const char *tr(enum str_id id);
int i18n_count(void);		/* languages in this build */
/* The language after 'code' in the list, wrapping around */
const char *i18n_next(const char *code);
