# ports/doom — Doom для bare_app (GPLv2)

Единственная часть репозитория под GPL: `doom_main.c` (платформенный слой) и сабмодуль
`doomgeneric` (движок, [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric)). Лицензия —
[LICENSE](LICENSE) (GNU GPL v2).

Отсюда собирается отдельная программа `bare_app_doom`; в сам `bare_app` из этого каталога
ничего не линкуется. bare_app запускает её как процесс и обменивается с ней только
кадрами (общая память) и кодами кнопок (сокет), см. `src/games/doom_ipc.h` и раздел
«Игры» в `docs/ARCHITECTURE.md`. Собирается, когда в Kconfig включён `GAME_DOOM`;
без него этот каталог и сабмодуль не нужны:

```sh
git submodule update --init ports/doom/doomgeneric
```
