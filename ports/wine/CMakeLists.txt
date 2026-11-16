# bare_app_wine: Windows games under Wine (GAME_WINE). A program of its own,
# like the other games: it talks to Wine's winebare.drv (a patch of the SDK
# package wine-wow64) through winebare_proto.h only, no Wine code is linked.
add_executable(bare_app_wine bare_app_wine.c ${PROJECT_SOURCE_DIR}/src/core/gp_names.c)
# next to bare_app: that is where it looks for the game
set_target_properties(bare_app_wine PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
# bare_app's src/ only for the exchange: games/game_ipc.h, core/input.h (headers),
# the buttons' names (core/gp_names.c) and the [keys] defaults (games/wine_defaults.h)
target_include_directories(bare_app_wine PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(bare_app_wine PRIVATE ${WARN})
target_link_options(bare_app_wine PRIVATE -Wl,--gc-sections)
target_link_libraries(bare_app_wine PRIVATE m)
install(TARGETS bare_app_wine RUNTIME DESTINATION bin)
