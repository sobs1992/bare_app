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
	[S_BRIGHTNESS]      = { "Brightness" RU("Яркость") },
	[S_NOT_AVAILABLE]   = { "not available" RU("недоступна") },
	[S_CHANGE_BACK]     = { "change    B  back" RU("изменить    B  назад") },
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
	[S_DOOM]            = { "Doom" },
	[S_DOOM_HINT]       = { "F1  menu    F5  leave" RU("F1  меню    F5  выйти") },
	[S_DOOM_NO_WAD_FMT] = { "No Doom IWAD file:\n%s\nB  back"
				RU("Нет IWAD-файла Doom:\n%s\nB  назад") },
	[S_HOMM3]           = { "Heroes III" RU("Герои III") },
	[S_HOMM3_HINT]      = { "L1  zoom    F5  leave\nL2 R2  volume"
				RU("L1  масштаб    F5  выйти\nL2 R2  громкость") },
	[S_GAME_LOADING]    = { "Loading..." RU("Загрузка...") },
	[S_GAME_MISSING_FMT] = { "Not found:\n%s\nB  back" RU("Не найдено:\n%s\nB  назад") },
	[S_GAME_FAILED]     = { "The game did not start, see the log\nB  back"
				RU("Игра не запустилась, подробности в логе\nB  назад") },
	[S_GAME_EXIT_FMT]   = { "The game stopped with an error (status %d), see the log\nB  back"
				RU("Игра завершилась с ошибкой (код %d), подробности в логе\nB  назад") },
	[S_GAME_SIGNAL_FMT] = { "The game crashed (signal %d)\nB  back"
				RU("Игра упала (сигнал %d)\nB  назад") },
	[S_VOLUME]          = { "Volume" RU("Громкость") },
	[S_OUTPUT]          = { "Output" RU("Вывод звука") },
	[S_OUT_AUTO]        = { "Auto" RU("Авто") },
	[S_OUT_SPEAKER]     = { "Speaker" RU("Динамик") },
	[S_OUT_HEADPHONES]  = { "Headphones" RU("Наушники") },
	[S_OUT_AUTO_NOW_FMT] = { "Auto: %s" RU("Авто: %s") },
	[S_NOW_SPEAKER]     = { "speaker" RU("динамик") },
	[S_NOW_HEADPHONES]  = { "headphones" RU("наушники") },
	[S_WIFI]            = { "Wi-Fi" },
	[S_WIFI_OFF]        = { "off" RU("выключен") },
	[S_WIFI_UNAVAILABLE] = { "not available" RU("недоступен") },
	[S_WIFI_DISCONNECTED] = { "not connected" RU("не подключён") },
	[S_WIFI_CONNECTING_FMT] = { "connecting to %s..." RU("подключение к %s...") },
	[S_WIFI_NO_IP_FMT]  = { "%s, getting an address..." RU("%s, получение адреса...") },
	[S_WIFI_NO_DAEMON]  = { "Wi-Fi is not available: no adapter, or wpa_supplicant is not running"
				RU("Wi-Fi недоступен: нет адаптера или не запущен wpa_supplicant") },
	[S_NETWORKS]        = { "Networks" RU("Сети") },
	[S_SEARCHING]       = { "searching..." RU("поиск...") },
	[S_NO_NETWORKS]     = { "No networks found" RU("Сети не найдены") },
	[S_SAVED]           = { "saved" RU("сохранена") },
	[S_OPEN]            = { "open" RU("открытая") },
	[S_WIFI_HINT]       = { "A  connect    X  forget    B  back"
				RU("A  подключить    X  забыть    B  назад") },
	[S_WIFI_FORGET_AGAIN_FMT] = { "X again: forget %s" RU("X ещё раз: забыть %s") },
	[S_WIFI_KEY]        = { "Network key" RU("Ключ сети") },
	[S_WIFI_KEY_HINT_FMT] = { "X  delete    %s  connect    B  back"
				  RU("X  стереть    %s  подключить    B  назад") },
	[S_WIFI_KEY_SHORT]  = { "The key has 8 to 63 characters" RU("В ключе от 8 до 63 символов") },
	[S_WIFI_JOINED]     = { "Connected" RU("Подключено") },
	[S_WIFI_WRONG_KEY]  = { "Wrong key" RU("Неверный ключ") },
	[S_WIFI_FAILED]     = { "Could not connect" RU("Не удалось подключиться") },
	[S_QUIT_HOLD]       = { "Keep holding to quit" RU("Удерживайте, чтобы выйти") },
	[S_POWER_HOLD]      = { "Keep holding to turn off" RU("Удерживайте, чтобы выключить") },
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
