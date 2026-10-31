/*
 * UI strings (view side, toolkit independent). The language is chosen at
 * start; a string missing in a language falls back to English.
 */
#pragma once

enum str_id {
	S_INPUT_TEST, S_SETTINGS, S_LAYERS, S_QUIT,
	S_BRIGHTNESS, S_NOT_AVAILABLE, S_CHANGE_BACK,
	S_TWICE_BACK,
	S_LAYERS_VIDEO, S_LAYERS_NO_VIDEO,
	S_BATTERY_LOW_FMT,	/* "%d" */
	S_CHARGER_CONNECTED, S_CHARGER_DISCONNECTED,
	S_LANGUAGE,
	S_LANG_NAME,		/* name of the current language in itself */
	S_DOOM, S_DOOM_HINT,
	S_DOOM_NO_WAD_FMT,	/* "%s": IWAD path */
	S_HOMM3, S_HOMM3_HINT,
	S_GAME_LOADING,
	S_GAME_MISSING_FMT,	/* "%s": path */
	S_GAME_FAILED,
	S_GAME_EXIT_FMT,	/* "%d": exit status */
	S_GAME_SIGNAL_FMT,	/* "%d": signal */
	S_VOLUME, S_OUTPUT, S_OUT_AUTO, S_OUT_SPEAKER, S_OUT_HEADPHONES,
	S_OUT_AUTO_NOW_FMT,	/* "%s": S_NOW_* */
	S_NOW_SPEAKER, S_NOW_HEADPHONES,
	S_WIFI, S_WIFI_OFF, S_WIFI_UNAVAILABLE, S_WIFI_DISCONNECTED,
	S_WIFI_CONNECTING_FMT,	/* "%s": network */
	S_WIFI_NO_IP_FMT,	/* "%s": network */
	S_WIFI_NO_DAEMON,
	S_NETWORKS, S_SEARCHING, S_NO_NETWORKS, S_SAVED, S_OPEN,
	S_WIFI_HINT,
	S_WIFI_FORGET_AGAIN_FMT,	/* "%s": network */
	S_WIFI_KEY,
	S_WIFI_KEY_HINT_FMT,	/* "%s": the OK key's symbol */
	S_WIFI_KEY_SHORT,
	S_WIFI_JOINED, S_WIFI_WRONG_KEY, S_WIFI_FAILED,
	S_COUNT
};

/* "en", "ru"; returns -1 (and keeps English) for an unknown language */
int i18n_set(const char *lang);
const char *i18n_lang(void);
const char *tr(enum str_id id);
int i18n_count(void);		/* languages in this build */
/* The language after 'code' in the list, wrapping around */
const char *i18n_next(const char *code);
