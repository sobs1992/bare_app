#include "i18n.h"

#include <string.h>

#ifdef CONFIG_LANG_RU
#define RU(s) , s
enum { LANG_EN, LANG_RU, LANG_COUNT };
static const char *const names[LANG_COUNT] = { "en", "ru" };
#else
#define RU(s)
enum { LANG_EN, LANG_COUNT };
static const char *const names[LANG_COUNT] = { "en" };
#endif

static const char *const strings[S_COUNT][LANG_COUNT] = {
	[S_INPUT_TEST]      = { "Input test" RU("Тест ввода") },
	[S_SETTINGS]        = { "Settings" RU("Настройки") },
	[S_LAYERS]          = { "Layers" RU("Слои") },
	[S_QUIT]            = { "Quit" RU("Выход") },
	[S_PLATFORM_FMT]    = { "%s  (platform %s)" RU("%s  (платформа %s)") },
	[S_UNKNOWN_BOARD]   = { "unknown board" RU("неизвестная плата") },
	[S_BRIGHTNESS]      = { "Brightness" RU("Яркость") },
	[S_NOT_AVAILABLE]   = { "not available" RU("недоступна") },
	[S_BATTERY]         = { "Battery" RU("Батарея") },
	[S_CHANGE_BACK]     = { "change    B  back" RU("изменить    B  назад") },
	[S_UNKNOWN]         = { "unknown" RU("неизвестно") },
	[S_NONE]            = { "none" RU("нет") },
	[S_FULL]            = { "full" RU("заряжена") },
	[S_CHARGING]        = { "charging" RU("заряжается") },
	[S_DISCHARGING]     = { "discharging" RU("разряжается") },
	[S_CHARGER_ON]      = { ", charger on" RU(", зарядка подключена") },
	[S_TWICE_BACK]      = { "B twice: back" RU("B дважды: назад") },
	[S_LAYERS_VIDEO]    = { "Video layer: test pattern (hardware plane)\n"
				"UI layer: LVGL with alpha on top\n"
				"B  back"
				RU("Видеослой: тестовая таблица (аппаратная плоскость)\n"
				   "Интерфейс: LVGL с прозрачностью поверх\n"
				   "B  назад") },
	[S_LAYERS_NO_VIDEO] = { "This display has no video layer\n"
				"(fbdev, SDL or memory display)\n"
				"B  back"
				RU("У этого экрана нет видеослоя\n"
				   "(fbdev, SDL или экран в памяти)\n"
				   "B  назад") },
	[S_BATTERY_LOW_FMT] = { "Battery low: %d%%" RU("Батарея разряжена: %d%%") },
	[S_CHARGER_CONNECTED]    = { "Charger connected" RU("Зарядка подключена") },
	[S_CHARGER_DISCONNECTED] = { "Charger disconnected" RU("Зарядка отключена") },
	[S_LANGUAGE]        = { "Language" RU("Язык") },
	[S_LANG_NAME]       = { "English" RU("Русский") },
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

int i18n_count(void)
{
	return LANG_COUNT;
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
