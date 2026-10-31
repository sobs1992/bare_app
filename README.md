# bare_app

GUI-приложение для OdroidLinux SDK на LVGL. Первая цель — ODROID-GO Advance:
экран, кнопки и стик. Архитектура рассчитана на добавление плат и оборудования
и на замену LVGL другим тулкитом: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

Репозиторий подключается к SDK сабмодулем `apps/bare_app`, LVGL (v9.6.0) — сабмодуль
`third_party/lvgl`:

```sh
git submodule update --init --recursive apps/bare_app      # из корня SDK
```

## Сборка и запуск

На ПК. Платформа `pc` открывает окно SDL2 (если при сборке был найден `libsdl2-dev`),
`pc-mem` рисует в память — для тестов и сценариев без окна:

```sh
cmake -S . -B build-host && cmake --build build-host -j
cd build-host && ctest                 # юнит-тесты и smoke-тест без тулкита
./bare_app                             # симулятор в окне
echo "down a wait:500 f6" | BARE_APP_SHOT=shot.ppm ./bare_app -p pc-mem   # сценарий кнопок, снимок экрана
```

На ODROID-GO Advance через консоль (`scripts/oga-serial.py` SDK, плата на `/dev/ttyUSB0`).
Нужен собранный тулчейн прошивки (`make FW=oga rootfs` в SDK):

```sh
tools/oga.sh run            # собрать, скопировать в /tmp/bare_app (если изменился), запустить
tools/oga.sh key down a     # нажать кнопки
tools/oga.sh shot s.png     # снимок всех слоёв работающего приложения (SIGUSR1)
tools/oga.sh stop           # остановить, показать конец лога
```

Опции программы: `-p PLATFORM` (без него — по device tree), `-b lvgl|headless`,
`-L en|ru` — язык (или `BARE_APP_LANG`), `-v` — отладочный лог.
На OGA экран выводится через DRM с аппаратными плоскостями (видео под UI, экран «Layers»),
если DRM недоступен — через fbdev. Снимок: `kill -USR1 $(pidof bare_app)` → `/tmp/bare_app.ppm`
(или `BARE_APP_SHOT`).
Выход: F6 (`quit_combo` платформы), пункт Quit, SIGTERM/SIGINT.

Шрифты с кириллицей пересоздаются так (нужен Pillow):

```sh
for s in 14 20; do tools/gen_font.py --size $s --name ui_font_$s \
    --fallback lv_font_montserrat_$s -o src/gui/view/lvgl/fonts/ui_font_$s.c; done
```

## Управление

| Кнопка | Действие |
|---|---|
| крестовина / стик вверх-вниз | перемещение фокуса |
| влево / вправо | значение у виджета в фокусе (ползунок) |
| A | нажать |
| B | назад (на экране теста ввода — дважды) |
| L1 / R1 | предыдущий / следующий виджет |
| F6 | выход |

В симуляторе SDL: стрелки, X = A, Z/Esc = B, S = X, A = Y, Q/W = L1/R1, 1…6 = F1…F6.
