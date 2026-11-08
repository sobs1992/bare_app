# Как добавить игру в bare_app

Каждая игра bare_app — отдельная программа. bare_app запускает её на каждый сеанс,
передаёт сокет и общую память, показывает её кадры в видеослое и отдаёт ей геймпад.
Игра может упасть, но приложение останется работать. Лицензия игры не
распространяется на bare_app.

Путь зависит от того, на чём написана игра:

| Игра | Что сделать | Код bare_app |
|---|---|---|
| SDL2, есть в Buildroot или собирается пакетом | пакет + `game.ini` в образе | не меняется |
| SDL2, собрана отдельно и скопирована с ПК | каталог на `/games` с `game.ini` | не меняется |
| без SDL (свой движок) | связь с bare_app в игре + запись в реестре | один файл и строка в таблице |

Первые два пути работают благодаря SDL2 SDK: он собран с драйверами `bareapp`
(опция `bare-app.SDL=y` в прошивке). Почти все открытые порты под Linux используют
SDL2, поэтому обычно хватает первого пути.

## Что получает игра

**Экран.** Режим рабочего стола SDL — размер видеослоя: 480×320 на OGA, 854×480 на GO
Ultra. Окно любого размера вписывается в экран с сохранением пропорций и чёрными
полями. Полноэкранные режимы 320×200, 320×240, 640×400, 640×480 и 800×600 тоже
принимаются и вписываются так же. Программный рендер копируется в кадр. OpenGL ES 2/3
рисует GPU вне экрана, кадр читается после каждого `SDL_GL_SwapWindow`. Настольного
OpenGL нет. Кадры идут не чаще 60 в секунду, как при vsync.

**Геймпад.** Геймпад bare_app — один `SDL_GameController` с именем «bare_app
gamepad». Раскладка по умолчанию:

| Кнопка устройства | Кнопка контроллера |
|---|---|
| крестовина | dpup, dpdown, dpleft, dpright |
| A, B, X, Y | a, b, x, y (по надписям, не по положению) |
| L1, R1 | leftshoulder, rightshoulder |
| L2, R2 | lefttrigger, righttrigger (0 или полностью) |
| Select, F2 | back |
| Start, F1 | start |
| Menu | guide |
| F3, F4 | misc1, paddle1 |
| левый стик, правый стик | leftx/lefty, rightx/righty |

F5 сворачивает игру, удержание F6 закрывает её, кнопки громкости меняют громкость.
Эти кнопки остаются у bare_app. Для игр без поддержки контроллера кнопки можно
превратить в клавиши строкой `keys`. Тогда направления левого стика тоже нажимают
клавиши крестовины.

**Звук.** `SDL_AUDIODRIVER=alsa` и `AUDIODEV` со звуковой картой платы.

**Каталог данных.** `/data/<игра>`: это HOME игры и её рабочий каталог,
`XDG_DATA_HOME`, `XDG_CONFIG_HOME` и `XDG_CACHE_HOME` лежат внутри. Туда попадает всё,
что игра пишет через `SDL_GetPrefPath()` или в `~`. Раздел `/data` — ext4, он
переживает перезапись образа. `/games` смонтирован только для чтения.

**Язык.** `BARE_APP_LANG` (`en`, `ru`) и подстановка `{lang}` в аргументах.

**Выход.** Когда игра завершается сама с кодом 0, экран игры закрывается. С другим
кодом или по сигналу экран показывает причину. Когда игру закрывают удержанием F6, она
получает SIGTERM, который SDL превращает в `SDL_QUIT`. Если через секунду игра ещё
работает, её убивает SIGKILL. Когда сокет bare_app закрывается, игра тоже получает
`SDL_QUIT`.

Вывод игры (stdout, stderr) попадает в журнал bare_app, `/var/log/bare_app.log`.

## game.ini

Пункт меню появляется для каждого файла:

- `/usr/share/bare_app/games/<игра>.ini` — так игру ставит пакет образа;
- `/games/<игра>/game.ini` — так приходит игра, скопированная с ПК.

`<игра>` — имя файла или каталога. От него зависит каталог данных `/data/<игра>`,
поэтому его лучше не менять после первого запуска. Список каталогов задаёт
`GAME_NATIVE_DIRS` в Kconfig. Файлы читаются заново при каждом показе главного меню.

Короткий пример:

```ini
name = My Game
exe = /usr/bin/mygame
args = --fullscreen
needs = /games/mygame/data.pak
keys = a=Space, b=Left Ctrl, start=Return, f1=Escape
hint = A  jump    B  fire    F1  menu\nF5  minimise    F6 (hold)  quit
hint.ru = A  прыжок    B  огонь    F1  меню\nF5  свернуть    F6 (держать)  выйти
```

Все ключи с пояснениями — в [game.ini](game.ini). Самые нужные:

| Ключ | Что задаёт |
|---|---|
| `name` | текст пункта меню |
| `exe` | программа; относительный путь — от каталога ini |
| `args` | аргументы; `"кавычки"` сохраняют пробелы |
| `needs` | файл, без которого игра не запустится; можно несколько строк. Если его нет, экран пишет «Не найдено: <путь>» |
| `keys` | кнопки как клавиши: `кнопка=имя клавиши SDL` через запятую |
| `gamepad` | другая раскладка контроллера: `f3=guide, l2=none` |
| `scale`, `filter` | `fit`, `stretch`, `integer`; `linear`, `nearest` |
| `hint`, `hint.ru` | подсказка после первого кадра, `\n` — новая строка |
| `cover` | картинка игры на главном экране (PNG); путь относительно ini. По умолчанию `cover.png` рядом с `game.ini`, а для `<id>.ini` — `<id>.png` |
| `volume_triggers` | `yes`: L2/R2 меняют громкость и не идут в игру |
| `[env]` | любые переменные окружения игры |

Имена кнопок: `up down left right a b x y l1 r1 l2 r2 select start menu f1`…`f6`.
Имена клавиш — как у `SDL_GetScancodeFromName()`: `Return`, `Escape`, `Space`,
`Left Ctrl`, `Right Shift`, `Tab`, `Comma`, `A`…`Z`, `F1`…`F12`.

В `args`, `dir` и `[env]` подставляются `{width}` и `{height}` (размер видеослоя),
`{lang}`, `{data}` (каталог данных) и `{dir}` (каталог ini).

## Пример: Chocolate Doom из Buildroot

Chocolate Doom уже есть в Buildroot и использует SDL2. Чтобы он появился в меню, в SDK
добавлено четыре вещи и ни одной строки кода.

**1. Описание игры** `board/common/buildroot/package/bare-app/games/chocolate-doom.ini`:

```ini
name = Chocolate Doom
exe = /usr/bin/chocolate-doom
args = -iwad /usr/share/games/doom/doom1.wad
needs = /usr/share/games/doom/doom1.wad
keys = up=Up, down=Down, left=Left, right=Right, a=Right Ctrl, b=Space, x=Right Shift, y=Return, l1=Comma, r1=Period, select=Tab, start=Escape, f1=Escape, f2=Tab, f3=Y, f4=N
hint = A  fire    B  use    X  run    Y  enter    L1/R1  strafe\nF1  menu    F2  map    F3/F4  yes/no    F5  minimise    F6 (hold)  quit
```

Клавиши в `keys` — стандартные клавиши Doom: огонь на правом Ctrl, «использовать» на
пробеле, бег на правом Shift, шаг вбок на запятой и точке. Контроллер Chocolate Doom
по умолчанию не читает, поэтому кнопки идут как клавиши.

**2. Опция** в `board/common/buildroot/package/bare-app/Config.in`:

```kconfig
config BR2_PACKAGE_BARE_APP_CHOCOLATE_DOOM
	bool "Chocolate Doom (an SDL2 game by its game.ini)"
	depends on BR2_PACKAGE_BARE_APP_SDL
	depends on BR2_USE_MMU # chocolate-doom
	select BR2_PACKAGE_CHOCOLATE_DOOM
	select BR2_PACKAGE_SDL2_MIXER # sound and OPL music
	select BR2_PACKAGE_DOOM1_WAD
```

**3. Установка ini** — строка в `bare-app.mk`, остальное делает общий хук:

```make
BARE_APP_GAME_INIS-$(BR2_PACKAGE_BARE_APP_CHOCOLATE_DOOM) += chocolate-doom.ini
```

**4. Прошивка** — опции в `configs/ogu_defconfig`:

```make
APPS_CONFIG := ... bare-app.SDL=y bare-app.CHOCOLATE_DOOM=y
```

На GO Ultra игра идёт на GPU со звуком, 34–35 кадров в секунду (темп Doom — 35
тиков), процесс занимает около половины ядра.

## Пример: новая SDL2-игра, которой нет в Buildroot

Пусть игра называется `mygame`, собирается CMake и нуждается в SDL2 и SDL2_mixer.
Её данные в образ не входят: пользователь копирует их в `/games/mygame`.

**1. Пакет Buildroot** в `board/common/buildroot/package/mygame/`.

`Config.in`:

```kconfig
config BR2_PACKAGE_MYGAME
	bool "mygame"
	depends on !BR2_STATIC_LIBS # sdl2
	select BR2_PACKAGE_SDL2
	select BR2_PACKAGE_SDL2_MIXER
	help
	  My Game, an SDL2 game. The game data is not included: copy
	  it to mygame/ of the games partition (GAMES, /games).

	  https://example.org/mygame
```

`mygame.mk`:

```make
MYGAME_VERSION = 1.2.3
MYGAME_SITE = $(call github,author,mygame,v$(MYGAME_VERSION))
MYGAME_LICENSE = GPL-2.0+
MYGAME_LICENSE_FILES = LICENSE
MYGAME_DEPENDENCIES = sdl2 sdl2_mixer
MYGAME_CONF_OPTS = -DDATA_DIR=/games/mygame

$(eval $(cmake-package))
```

Строку `source "$BR2_EXTERNAL_SDK_PATH/package/mygame/Config.in"` добавить в
`board/common/buildroot/Config.in`. Хеш архива кладут в `mygame.hash`, им Buildroot
проверяет загрузку.

**2. Описание** `board/common/buildroot/package/bare-app/games/mygame.ini`:

```ini
name = My Game
exe = /usr/bin/mygame
; данные не в образе: без них меню скажет, что не найдено
needs = /games/mygame/data.pak
args = --data /games/mygame
hint = A  jump    B  fire    Start  menu\nF5  minimise    F6 (hold)  quit
hint.ru = A  прыжок    B  огонь    Start  меню\nF5  свернуть    F6 (держать)  выйти
```

Если игра понимает контроллер SDL, строка `keys` не нужна. Если игра рисует меньшим
окном и мелкие пиксели размываются, помогает `filter = nearest` или
`scale = integer`.

**3. Опция и установка ini** — как у Chocolate Doom:

```kconfig
config BR2_PACKAGE_BARE_APP_MYGAME
	bool "My Game"
	depends on BR2_PACKAGE_BARE_APP_SDL
	select BR2_PACKAGE_MYGAME
```

```make
BARE_APP_GAME_INIS-$(BR2_PACKAGE_BARE_APP_MYGAME) += mygame.ini
```

**4. Прошивка и сборка:**

```make
APPS_CONFIG += bare-app.MYGAME=y      # configs/ogu_defconfig
```

```sh
make rootfs        # пакет, ini, bare_app
make               # или весь образ
```

Пересобирать образ для проверки не обязательно. Достаточно скопировать на плату
программу и ini и вернуться в главное меню:

```sh
T=output/ogu/buildroot/target
tar -C $T -cf - usr/bin/mygame usr/share/bare_app/games/mygame.ini | \
    ssh root@192.168.7.2 'tar -C / -xf -'
```

## Пример: игра, скопированная на /games

Готовую программу можно положить на раздел игр без пакета, например через экран
«USB-диск». Программа должна быть собрана под aarch64 тулчейном SDK
(`make rootfs-sdk` или `output/<прошивка>/buildroot/host/bin/aarch64-buildroot-linux-gnu-gcc`)
и использовать библиотеки образа.

```text
/games/mygame/
    game.ini
    mygame          программа
    data/           её файлы
```

```ini
name = My Game
exe = mygame
dir = {dir}
needs = data
```

`exe` и `needs` здесь относительные: они считаются от каталога ini. `dir = {dir}`
делает рабочим каталогом сам каталог игры, если игра ищет данные рядом с собой.
Писать туда она не сможет: `/games` только для чтения. Сохранения и настройки должны
идти в HOME, то есть в `/data/mygame`.

## Пример: игра со своим движком без SDL

Если в движке нет SDL или его вывод нужно делать по-своему (лупа в Героях III, рендер
Half-Life), игра говорит с bare_app напрямую, а в bare_app добавляется запись реестра.

**Сторона игры.** Протокол — `src/games/game_ipc.h`: игра получает дескриптор 3
(сокет `SOCK_SEQPACKET`) и 4 (memfd с тремя кадрами XRGB8888). Сторона игры уже
написана в `src/games/game_ipc_client.h` (только заголовок, C и C++). Оба заголовка под
лицензией MIT, их можно включать в игру с любой лицензией. В Buildroot они лежат в
`$(STAGING_DIR)/usr/include/bare_app/` (`games/`, `core/`), пакет игры зависит от
`bare-app`. Минимальный цикл:

```c
#include <poll.h>

#include "core/input.h"             /* enum gp_button, enum gp_axis */
#include "games/game_ipc_client.h"

struct game_link app;
struct game_msg m;
int n;

if (game_link_open(&app, false) < 0)
	exit(2);                    /* запущена не из bare_app */
for (;;) {
	/* кнопки и оси; ждать их можно poll() на GAME_IPC_SOCK */
	while ((n = game_link_recv(&m)) > 0) {
		if (m.type == GAME_MSG_BUTTON)
			on_button(m.code, m.pressed, m.flags & GAME_BTN_STICK);
		else if (m.type == GAME_MSG_AXIS)
			on_axis(m.code, m.value);   /* -32767..32767 */
	}
	if (n < 0)
		break;              /* bare_app закрыл сокет: выйти */

	/* кадр app.shm->width x app.shm->height, строка app.shm->stride байт */
	draw(game_link_frame(&app));
	if (game_link_publish(&app) < 0)
		break;
}
```

Кадр уже должен быть размера слоя: масштабирует игра. Неизменившиеся кадры лучше не
отправлять. Готовые образцы: `tests/fake_game.c` (самый короткий),
`ports/doom/doom_main.c`, `ports/wine/bare_app_wine.c`.

**Сторона bare_app.** Файл `src/games/mygame.c`:

```c
/*
 * My Game (GAME_MYGAME): mygame with its bare_app link (SDK package mygame).
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "core/log.h"
#include "game.h"

#define TAG "mygame"

static const char *mygame_missing(int entry)
{
	(void)entry;
	if (access(CONFIG_GAME_MYGAME_PROGRAM, X_OK) < 0)
		return CONFIG_GAME_MYGAME_PROGRAM;
	if (access("/games/mygame/data.pak", R_OK) < 0)
		return "/games/mygame/data.pak";
	return NULL;
}

static struct game_proc *mygame_start(int entry, const struct game_env *ge)
{
	char data[PATH_MAX];
	char *const argv[] = { (char *)"mygame", (char *)"--lang", (char *)ge->lang, NULL };
	const char *missing = mygame_missing(entry);
	struct game_vars env = { 0 };

	if (missing) {
		LOGE(TAG, "%s: %s", missing, strerror(ENOENT));
		errno = ENOENT;
		return NULL;
	}
	/* BARE_APP_MYGAME_DATA подменяет каталог при проверке на ПК */
	if (game_proc_mkdir(game_setting("BARE_APP_MYGAME_DATA", "/data/mygame"),
			    data, sizeof(data)) < 0)
		return NULL;
	/* окружение поверх окружения bare_app; для SDL-игры звук — game_vars_sdl_audio() */
	game_vars_add(&env, "MYGAME_DATA=%s", data);
	game_vars_add(&env, "MYGAME_PCM=%s", ge->pcm ? ge->pcm : "");
	struct game_spawn s = { .path = CONFIG_GAME_MYGAME_PROGRAM, .argv = argv, .env = env.v,
				.dir = data, .width = ge->width, .height = ge->height };

	return game_proc_start(&s);
}

const struct game_def game_mygame = {
	.id = "mygame",
	.title = S_MYGAME,
	.hint = S_MYGAME_HINT,
	.trigger_volume = true,		/* L2/R2 — громкость */
	.missing = mygame_missing,
	.start = mygame_start,
};
```

Остальное — по строке в нескольких местах:

- `src/games/games.c`: `extern const struct game_def game_mygame;` и строка
  `&game_mygame,` под `#ifdef CONFIG_GAME_MYGAME` там, где игра должна стоять в меню;
- `Kconfig`, меню «Games»:

  ```kconfig
  config GAME_MYGAME
  	bool "My Game (separate program)"
  	depends on VIEW_LVGL
  	select GAMES

  config GAME_MYGAME_PROGRAM
  	string "Program"
  	depends on GAME_MYGAME
  	default "/usr/bin/mygame"
  ```

- `CMakeLists.txt`: `add_if(CONFIG_GAME_MYGAME src/games/mygame.c)`;
- `src/gui/view/core/i18n.h` и `i18n.c`: `S_MYGAME`, `S_MYGAME_HINT` на английском и
  русском;
- `configs/ogu_defconfig` и `oga_defconfig`: `CONFIG_GAME_MYGAME=y` для сборки без
  Buildroot (`tools/oga.sh`). В образе игру включает опция пакета bare-app: строка
  `CONFIG_GAME_MYGAME=$(call BARE_APP_YN,$(BR2_PACKAGE_BARE_APP_MYGAME))` в
  `BARE_APP_KCONFIG` в `bare-app.mk`, тогда она включается в одном месте,
  `APPS_CONFIG` прошивки;
- если игре нужно знать что-то о плате, это строка `"mygame.ключ=значение"` в
  `game_opts` платформы (`src/platforms/*.c`), игра читает её `platform_opt()`, как
  Герои читают `homm3.keys`.

Поля `struct game_def`, которые здесь не понадобились:

| Поле | Когда нужно |
|---|---|
| `missing_fmt` | своё сообщение вместо «Не найдено: %s» (Doom: «Нет IWAD-файла») |
| `entries`, `info` | у игры несколько пунктов, найденных во время работы (Windows-игры, игры из ini) |
| `hint_for`, `minimize_for` | подсказка и кнопка сворачивания зависят от платы (Герои на GO Ultra сворачиваются F1) |

Экран, меню, сворачивание, диалог закрытия и сообщения об ошибках bare_app делает
сам, по таблице. Подробнее об устройстве — раздел «Игры в отдельном процессе» в
[ARCHITECTURE.md](ARCHITECTURE.md).

## Проверка на ПК

Как устроена связь, проверяет `ctest` в `build-host`: тест `game_smoke` запускает
`tests/fake_game.c` по его `game.ini` из меню, нажимает кнопки, сворачивает игру,
возвращает её и проверяет оба выхода. Свою игру можно погонять так же.

Без платы игру можно проверить в симуляторе. Нужен SDL2 2.32 с патчем SDK:

```sh
tar xzf dl/sdl2/SDL2-2.32.10.tar.gz && cd SDL2-2.32.10
patch -p1 < board/common/buildroot/package/bare-app/sdl2/0001-*.patch
cp apps/bare_app/src/games/game_ipc.h apps/bare_app/src/games/game_ipc_client.h \
   apps/bare_app/src/core/input.h src/video/bareapp/
mkdir build && cd build && ../configure --prefix=$HOME/sdl-bareapp --enable-bareapp
make -j8 && make install
```

Игру собрать с этой библиотекой или указать её в `[env]`:
`LD_LIBRARY_PATH = /home/me/sdl-bareapp/lib`. Видеослой есть только у платформы
`pc-mem`, кнопки подаются строкой на stdin, снимок пишется при выходе:

```sh
echo "wait:800 down down down a wait:3000 a b" | \
    BARE_APP_NATIVE_DIRS=$PWD/my-games BARE_APP_NATIVE_DATA=/tmp/games-data \
    BARE_APP_SHOT=/tmp/shot.ppm timeout 8 build-host/bare_app -p pc-mem
tools/ppm2png.py /tmp/shot.ppm /tmp/shot.png
```

## Если что-то не так

| Признак | Где смотреть |
|---|---|
| игры нет в меню | ini лежит не там или в нём нет `exe`; в журнале `native: ...: no exe` |
| «Не найдено: <путь>» | `exe` не исполняемый или нет файла из `needs` |
| игра стартует и сразу закрывается с ошибкой | её вывод в `/var/log/bare_app.log` |
| чёрный экран, «Загрузка...» не проходит | игра выбрала другой видеодрайвер: в `[env]` не должно быть `SDL_VIDEODRIVER`; подробности драйвера — `SDL_LOGGING = video=debug` в `[env]` |
| игра не видит контроллер | она читает `/dev/input` сама, а не через SDL, или знает только клавиатуру: тогда помогает `keys` |
| медленно на OGA | рендер GPU читается медленнее, чем рисует процессор: `SDL_RENDER_DRIVER = software` в `[env]` |
| изменения патча SDL2 не попали в образ | Buildroot сам SDL2 не пересобирает: `make rootfs-make BR=sdl2-dirclean`, затем `make rootfs` |
