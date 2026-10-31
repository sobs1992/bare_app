/*
 * UI strings (view side, toolkit independent). The language is chosen at
 * start; a string missing in a language falls back to English.
 */
#pragma once

enum str_id {
	S_INPUT_TEST, S_SETTINGS, S_LAYERS, S_QUIT,
	S_PLATFORM_FMT,		/* "%s  (platform %s)": model, platform */
	S_UNKNOWN_BOARD,
	S_BRIGHTNESS, S_NOT_AVAILABLE, S_BATTERY, S_CHANGE_BACK,
	S_UNKNOWN, S_NONE, S_FULL, S_CHARGING, S_DISCHARGING, S_CHARGER_ON,
	S_TWICE_BACK,
	S_LAYERS_VIDEO, S_LAYERS_NO_VIDEO,
	S_BATTERY_LOW_FMT,	/* "%d" */
	S_CHARGER_CONNECTED, S_CHARGER_DISCONNECTED,
	S_LANGUAGE,
	S_LANG_NAME,		/* name of the current language in itself */
	S_COUNT
};

/* "en", "ru"; returns -1 (and keeps English) for an unknown language */
int i18n_set(const char *lang);
const char *i18n_lang(void);
const char *tr(enum str_id id);
int i18n_count(void);		/* languages in this build */
/* The language after 'code' in the list, wrapping around */
const char *i18n_next(const char *code);
