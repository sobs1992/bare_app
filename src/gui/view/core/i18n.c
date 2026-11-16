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
	[S_BENCH]           = { "Stress test" RU("Стресс-тест") },
	[S_BENCH_HINT]      = { "B  quit    A  hide the text\nF5  minimise (the test pauses)"
				RU("B  выход    A  скрыть текст\nF5  свернуть (тест на паузе)") },
	[S_BRIGHTNESS]      = { "Brightness" RU("Яркость") },
	[S_NOT_AVAILABLE]   = { "not available" RU("недоступна") },
	[S_STICK_CAL_REST]  = { "Let go of the sticks...\nB  cancel"
				RU("Отпустите стики...\nB  отмена") },
	[S_STICK_CAL_TURN]  = { "Turn the sticks along their edge, slowly, 2-3 times\n"
				"A  save    Y  reset    B  cancel"
				RU("Медленно обведите стиками край, 2-3 круга\n"
				   "A  сохранить    Y  сброс    B  отмена") },
	[S_STICK_CAL_SAVED] = { "Sticks calibrated" RU("Стики откалиброваны") },
	[S_STICK_CAL_RESET] = { "Calibration reset" RU("Калибровка сброшена") },
	[S_STICK_CAL_NOT_SAVED] = { "Could not save the calibration"
				    RU("Не удалось сохранить калибровку") },
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
	[S_DOOM_HINT]       = { "F1  menu    F5  minimise\nF5 (hold)  system monitor\nF6 (hold)  quit"
				RU("F1  меню    F5  свернуть\nF5 (держать)  монитор системы\n"
				   "F6 (держать)  выйти") },
	[S_DOOM_NO_WAD_FMT] = { "No Doom IWAD file:\n%s\nB  back"
				RU("Нет IWAD-файла Doom:\n%s\nB  назад") },
	[S_HOMM3]           = { "Heroes III" RU("Герои III") },
	[S_HOMM3_HINT]      = { "L1  zoom    F3  cancel    F4  options\n"
				"F1  Ctrl    F2  Shift    L2 R2  volume\n"
				"F5  minimise    F6 (hold)  quit\n"
				"F5 (hold)  system monitor"
				RU("L1  масштаб    F3  отмена    F4  меню\n"
				   "F1  Ctrl    F2  Shift    L2 R2  громкость\n"
				   "F5  свернуть    F6 (держать)  выйти\n"
				   "F5 (держать)  монитор системы") },
	[S_HOMM3_HINT_OGU]  = { "F1  minimise    F2  Ctrl (one off)    F3  Shift (split)\n"
				"F4  accept    F5  cancel / end turn    F6 (hold)  quit\n"
				"F5 (hold)  system monitor"
				RU("F1  свернуть    F2  Ctrl (одного)    F3  Shift (поровну)\n"
				   "F4  да    F5  отмена / конец хода    F6 (держать)  выйти\n"
				   "F5 (держать)  монитор системы") },
	[S_HALFLIFE]        = { "Half-Life" },
	[S_HALFLIFE_HINT]   = { "F1  menu    F2  pause    F3  quick save    F4  quick load\n"
				"F5  minimise    F6 (hold)  quit\n"
				"F5 (hold)  system monitor"
				RU("F1  меню    F2  пауза    F3  быстрое сохранение    F4  загрузка\n"
				   "F5  свернуть    F6 (держать)  выйти\n"
				   "F5 (держать)  монитор системы") },
	[S_WINE_HINT]       = { "Stick  mouse    A  left click    B  right click    Start  Esc\n"
				"F5  minimise    F6 (hold)  quit\n"
				"F5 (hold)  system monitor"
				RU("Стик  мышь    A  левая кнопка    B  правая    Start  Esc\n"
				   "F5  свернуть    F6 (держать)  выйти\n"
				   "F5 (держать)  монитор системы") },
	[S_NATIVE_HINT]     = { "F5  minimise    F6 (hold)  quit\nF5 (hold)  system monitor"
				RU("F5  свернуть    F6 (держать)  выйти\nF5 (держать)  монитор системы") },
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
	[S_GAME_QUIT_HOLD]  = { "Keep holding to quit the game"
				RU("Удерживайте, чтобы выйти из игры") },
	[S_GAME_MINIMIZED_FMT] = { "%s is minimised.\nClose it and start %s?"
				   RU("Игра «%s» свёрнута.\nЗакрыть её и запустить «%s»?") },
	[S_CANCEL]          = { "Cancel" RU("Отмена") },
	[S_CLOSE_AND_START] = { "Close and start" RU("Закрыть и запустить") },
	[S_USB_DISK]        = { "USB disk" RU("USB-диск") },
	[S_USB_DISK_STARTING] = { "Opening the games partition to the computer..."
				  RU("Открываю раздел с играми компьютеру...") },
	[S_USB_DISK_NO_HOST] = { "Connect the console to a computer with a USB-C cable: "
				 "the games partition shows up there as the disk GAMES."
				 RU("Подключите приставку к компьютеру кабелем USB-C: "
				    "раздел с играми появится там как диск GAMES.") },
	[S_USB_DISK_SHARED] = { "The games partition is the disk GAMES on the computer.\n\n"
				"Copy the games onto it (the folders heroes3, halflife, doom), "
				"then eject the disk on the computer: "
				"the console takes it back by itself."
				RU("Раздел с играми открыт на компьютере как диск GAMES.\n\n"
				   "Скопируйте на него игры (папки heroes3, halflife, doom), "
				   "затем извлеките диск на компьютере: "
				   "приставка заберёт его сама.") },
	[S_USB_DISK_AGAIN]  = { "Eject the disk on the computer first.\nB again: disconnect anyway"
				RU("Сначала извлеките диск на компьютере.\n"
				   "B ещё раз: отключить всё равно") },
	[S_USB_DISK_EJECTED] = { "The disk is back on the console: the games are ready."
				 RU("Диск вернулся на приставку: игры доступны.") },
	[S_USB_DISK_BUSY]   = { "The games partition is in use: close the game and try again."
				RU("Раздел с играми занят: закройте игру и попробуйте снова.") },
	[S_USB_DISK_FAILED] = { "Could not open the games partition to the computer."
				RU("Не удалось открыть раздел с играми компьютеру.") },
	[S_USB_DISK_UNAVAILABLE] = { "Not available on this device."
				     RU("На этом устройстве недоступно.") },
	[S_USB_DISK_HINT_SHARED] = { "B  disconnect" RU("B  отключить") },
	[S_USB_DISK_HINT_RETRY]  = { "A  connect again    B  back"
				     RU("A  подключить снова    B  назад") },
	[S_USB_DISK_HINT_BACK]   = { "B  back" RU("B  назад") },
	[S_HOME_START]      = { "Play" RU("Играть") },
	[S_HOME_OPEN]       = { "Open" RU("Открыть") },
	[S_HOME_RESUME]     = { "Resume" RU("Продолжить") },
	[S_HOME_NO_GAMES]   = { "No games yet" RU("Игр пока нет") },
	[S_LIBRARY]         = { "All games" RU("Все игры") },
	[S_HOME_NOT_INSTALLED] = { "not installed" RU("не установлена") },
	[S_SET_SCREEN]      = { "Screen" RU("Экран") },
	[S_SET_SOUND]       = { "Sound" RU("Звук") },
	[S_SET_INTERNET]    = { "Internet" RU("Интернет") },
	[S_SET_SYSTEM]      = { "System" RU("Система") },
	[S_IP_ADDRESS]      = { "IP address" RU("IP-адрес") },
	[S_DEVICE]          = { "Device" RU("Устройство") },
	[S_PLATFORM]        = { "Platform" RU("Платформа") },
	[S_BATTERY]         = { "Battery" RU("Батарея") },
	[S_CHARGING]        = { "charging" RU("заряжается") },
	[S_BACK]            = { "Back" RU("Назад") },
	[S_OK]              = { "OK" },
	[S_CHANGE]          = { "Change" RU("Изменить") },
	[S_SET_GAMES]       = { "Games" RU("Игры") },
	[S_GAME_HINT]       = { "Controls at the start" RU("Подсказка при запуске") },
	[S_ON]              = { "On" RU("Вкл.") },
	[S_OFF]             = { "Off" RU("Выкл.") },
	[S_CALIBRATE]       = { "Calibrate" RU("Калибровка") },
	[S_CONNECT]         = { "Connect" RU("Подключиться") },
	[S_FORGET]          = { "Forget" RU("Забыть") },
	[S_CONTROLS]        = { "Controls" RU("Управление") },
	[S_RESET]           = { "Reset" RU("Сбросить") },
	[S_DEFAULT]         = { "Default" RU("Как было") },
	[S_CONTROLS_RESET_ALL] = { "All controls as they were" RU("Всё управление как было") },
	[S_STREAM]          = { "Stream (RTSP)" RU("Трансляция (RTSP)") },
	[S_STREAM_FAILED]   = { "did not start" RU("не запустилась") },
	[S_LEFT_STICK]      = { "Left stick" RU("Левый стик") },
	[S_RIGHT_STICK]     = { "Right stick" RU("Правый стик") },
	[S_DEAD_ZONE]       = { "Dead zone" RU("Мёртвая зона") },
	[S_SENSITIVITY]     = { "Sensitivity" RU("Чувствительность") },
	[S_SNAPBACK]        = { "Snap back" RU("Возврат в центр") },
	[S_SAVE]            = { "Save" RU("Сохранить") },
	[S_STICK_TUNE_SAVED] = { "Stick settings saved" RU("Настройки стиков сохранены") },
	[S_INVERT_X]        = { "invert X" RU("инверсия X") },
	[S_INVERT_Y]        = { "invert Y" RU("инверсия Y") },
	[S_BATTERY_PLUG_IN] = { "Plug the charger in" RU("Подключите зарядку") },
	[S_HIBERNATE]       = { "Hibernation" RU("Гибернация") },
	[S_DATE_TIME]       = { "Date and time" RU("Дата и время") },
	[S_FIELD]           = { "Field" RU("Поле") },
	[S_GYRO]            = { "Gyro aiming" RU("Прицел гироскопом") },
	[S_GYRO_SENS]       = { "Gyro sensitivity" RU("Чувствительность гироскопа") },
	[S_MOTION]          = { "Motion sensor" RU("Датчик движения") },
	[S_MOTION_NONE]     = { "Not connected: plug the motion sensor into the USB port"
				RU("Не подключён: вставьте датчик движения в USB") },
	[S_MOTION_RATE_FMT] = { "Connected: %s, %d samples a second" RU("Подключён: %s, %d измерений в секунду") },
	[S_MOTION_ZERO_WAIT] = { "Put the device down for a second: the gyro's zero is measured"
				 RU("Положите консоль на секунду: нужно измерить ноль гироскопа") },
	[S_MOTION_ZERO_OK]  = { "Gyro zero measured" RU("Ноль гироскопа измерен") },
	[S_MOTION_AXES_NONE] = { "Up and down not calibrated: only left and right turn"
				 RU("Вверх-вниз не откалиброван: работает только влево-вправо") },
	[S_MOTION_AXES_OK]  = { "Up and down calibrated" RU("Вверх-вниз откалиброван") },
	[S_MOTION_CAL_STILL] = { "Put the device on a table and do not touch it"
				 RU("Положите консоль на стол и не трогайте") },
	[S_MOTION_CAL_TILT] = { "Now hold it as in a game and tilt its top towards you"
				RU("Теперь возьмите её как в игре и наклоните верхним краем к себе") },
	[S_MOTION_CAL_TURNED] = { "That was a turn, not a tilt: once more"
				  RU("Это был поворот, а не наклон: ещё раз") },
	[S_MOTION_CAL_DONE] = { "Motion sensor calibrated" RU("Датчик движения откалиброван") },
	[S_MOTION_CAL_NONE] = { "Calibration removed" RU("Калибровка удалена") },
	[S_MOTION_TRY]      = { "Turn and tilt the device: the cross follows (X: back to the middle)"
				RU("Поворачивайте и наклоняйте консоль: крестик следует за ней (X — в центр)") },
	[S_PLUGGED]         = { "Plugged in" RU("Подкл.") },
	[S_UNPLUGGED]       = { "Unplugged" RU("Откл.") },
	[S_GYRO_INVERT_X]   = { "Invert X" RU("Инверсия X") },
	[S_GYRO_INVERT_Y]   = { "Invert Y" RU("Инверсия Y") },
	[S_MOTION_NO_ACCEL] = { "No accelerometer: its axes come from the calibration"
				RU("Нет акселерометра: оси задаёт калибровка") },
	[S_MOTION_NO_GYRO]  = { "No gyro: only up and down, by the tilt"
				RU("Нет гироскопа: только вверх-вниз, по наклону") },
	[S_MOTION_NO_AIM]   = { "Neither a gyro nor an accelerometer: no aiming"
				RU("Нет ни гироскопа, ни акселерометра: прицел недоступен") },
	[S_MOTION_AXES_NEEDED] = { "Not calibrated: aiming after the calibration (A)"
				   RU("Не откалиброван: прицел заработает после калибровки (A)") },
	[S_MOTION_CAL_TURN] = { "Now turn it to the left" RU("Теперь поверните её влево") },
	[S_MOTION_CAL_TILTED] = { "That was a tilt, not a turn: once more"
				  RU("Это был наклон, а не поворот: ещё раз") },
	[S_CTL_LCLICK]      = { "left click" RU("левый клик") },
	[S_CTL_RCLICK]      = { "right click" RU("правый клик") },
	[S_CTL_MCLICK]      = { "middle click" RU("средний клик") },
	[S_CTL_WHEEL_UP]    = { "wheel up" RU("колесо вверх") },
	[S_CTL_WHEEL_DOWN]  = { "wheel down" RU("колесо вниз") },
	[S_CTL_MAGNIFY]     = { "magnifier" RU("лупа") },
	[S_CTL_SPACE]       = { "Space" RU("Пробел") },
	[S_CTL_UP]          = { "Up" RU("Вверх") },
	[S_CTL_DOWN]        = { "Down" RU("Вниз") },
	[S_CTL_LEFT]        = { "Left" RU("Влево") },
	[S_CTL_RIGHT]       = { "Right" RU("Вправо") },
	[S_CTL_MOUSE]       = { "mouse" RU("мышь") },
	[S_CTL_ARROWS]      = { "arrow keys" RU("стрелки") },
	[S_CTL_WHEEL]       = { "mouse wheel" RU("колесо мыши") },
	[S_CTL_NUMPAD]      = { "numeric keypad" RU("цифровой блок") },
	[S_CTL_TURN]        = { "turning" RU("поворот") },
	[S_CTL_SCROLL]      = { "map scrolling" RU("прокрутка карты") },
	[S_CTL_CURSOR]      = { "cursor" RU("курсор") },
	[S_DOOM_FIRE]       = { "fire, Enter" RU("огонь, Enter") },
	[S_DOOM_USE]        = { "use, back" RU("открыть, назад") },
	[S_DOOM_RUN]        = { "run" RU("бег") },
	[S_DOOM_WEAPON]     = { "next weapon" RU("следующее оружие") },
	[S_DOOM_STRAFE_L]   = { "step left" RU("шаг влево") },
	[S_DOOM_STRAFE_R]   = { "step right" RU("шаг вправо") },
	[S_DOOM_MAP]        = { "map" RU("карта") },
	[S_DOOM_MENU]       = { "menu" RU("меню") },
	[S_DOOM_SAVE]       = { "save" RU("запись") },
	[S_DOOM_LOAD]       = { "load" RU("загрузка") },
	[S_DOOM_PAUSE]      = { "pause" RU("пауза") },
	[S_DOOM_FORWARD]    = { "forward" RU("вперёд") },
	[S_DOOM_BACK]       = { "back" RU("назад") },
	[S_DOOM_TURN_L]     = { "turn left" RU("поворот влево") },
	[S_DOOM_TURN_R]     = { "turn right" RU("поворот вправо") },
	[S_DOOM_MOVE]       = { "walk, turn" RU("ходьба, поворот") },
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
