#include "i18n.h"

#include <string.h>

enum { LANG_EN, LANG_RU, LANG_COUNT };

static const char *const names[LANG_COUNT] = { "en", "ru" };

static const char *const strings[S_COUNT][LANG_COUNT] = {
	[S_INPUT_TEST]      = { "Input test", "Тест ввода" },
	[S_SETTINGS]        = { "Settings", "Настройки" },
	[S_LAYERS]          = { "Layers", "Слои" },
	[S_QUIT]            = { "Quit", "Выход" },
	[S_PLATFORM_FMT]    = { "%s  (platform %s)", "%s  (платформа %s)" },
	[S_UNKNOWN_BOARD]   = { "unknown board", "неизвестная плата" },
	[S_BRIGHTNESS]      = { "Brightness", "Яркость" },
	[S_NOT_AVAILABLE]   = { "not available", "недоступна" },
	[S_BATTERY]         = { "Battery", "Батарея" },
	[S_CHANGE_BACK]     = { "change    B  back", "изменить    B  назад" },
	[S_UNKNOWN]         = { "unknown", "неизвестно" },
	[S_NONE]            = { "none", "нет" },
	[S_FULL]            = { "full", "заряжена" },
	[S_CHARGING]        = { "charging", "заряжается" },
	[S_DISCHARGING]     = { "discharging", "разряжается" },
	[S_CHARGER_ON]      = { ", charger on", ", зарядка подключена" },
	[S_TWICE_BACK]      = { "B twice: back", "B дважды: назад" },
	[S_LAYERS_VIDEO]    = { "Video layer: test pattern (hardware plane)\n"
				"UI layer: LVGL with alpha on top\n"
				"B  back",
				"Видеослой: тестовая таблица (аппаратная плоскость)\n"
				"Интерфейс: LVGL с прозрачностью поверх\n"
				"B  назад" },
	[S_LAYERS_NO_VIDEO] = { "This display has no video layer\n"
				"(fbdev, SDL or memory display)\n"
				"B  back",
				"У этого экрана нет видеослоя\n"
				"(fbdev, SDL или экран в памяти)\n"
				"B  назад" },
	[S_BATTERY_LOW_FMT] = { "Battery low: %d%%", "Батарея разряжена: %d%%" },
	[S_CHARGER_CONNECTED]    = { "Charger connected", "Зарядка подключена" },
	[S_CHARGER_DISCONNECTED] = { "Charger disconnected", "Зарядка отключена" },
	[S_LANGUAGE]        = { "Language", "Язык" },
	[S_LANG_NAME]       = { "English", "Русский" },
};

static int lang = LANG_EN;

int i18n_set(const char *name)
{
	int i;

	for (i = 0; i < LANG_COUNT; i++)
		if (name && !strcmp(name, names[i])) {
			lang = i;
			return 0;
		}
	return -1;
}

const char *i18n_lang(void)
{
	return names[lang];
}

const char *i18n_next(const char *code)
{
	int i;

	for (i = 0; i < LANG_COUNT; i++)
		if (code && !strcmp(code, names[i]))
			return names[(i + 1) % LANG_COUNT];
	return names[0];
}

const char *tr(enum str_id id)
{
	if (id < 0 || id >= S_COUNT)
		return "?";
	return strings[id][lang] ? strings[id][lang] : strings[id][LANG_EN];
}
