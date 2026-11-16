# bare_app_bench: the stress test (GAME_BENCH). A program of its own, like
# the games: OpenGL ES through EGL (Mesa's surfaceless platform), frames
# over games/game_ipc.h.
find_package(PkgConfig REQUIRED)
pkg_check_modules(BENCH_GL REQUIRED IMPORTED_TARGET egl glesv2)
add_executable(bare_app_bench bare_app_bench.c)
# next to bare_app: that is where it looks for it
set_target_properties(bare_app_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
# bare_app's src/ only for the exchange: games/game_ipc_client.h, core/input.h (headers)
target_include_directories(bare_app_bench PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(bare_app_bench PRIVATE ${WARN})
target_link_options(bare_app_bench PRIVATE -Wl,--gc-sections)
target_link_libraries(bare_app_bench PRIVATE PkgConfig::BENCH_GL Threads::Threads m)
install(TARGETS bare_app_bench RUNTIME DESTINATION bin)
