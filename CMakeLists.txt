cmake_minimum_required(VERSION 3.16)
# LVGL's build enables C++ and ASM for its optional parts
project(bare_app C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(BARE_APP_WITH_LVGL "Build the LVGL view backend" ON)
option(BARE_APP_WITH_SDL "Build the SDL2 simulator display (needs SDL2)" ON)
option(BARE_APP_BUILD_TESTS "Build host unit tests" ON)

set(WARN -Wall -Wextra -Wno-unused-parameter)
add_compile_options(-ffunction-sections -fdata-sections)

find_package(Threads REQUIRED)
find_package(PkgConfig)
if(BARE_APP_WITH_SDL AND PKG_CONFIG_FOUND)
	pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
endif()
if(NOT SDL2_FOUND)
	set(BARE_APP_WITH_SDL OFF)
endif()
message(STATUS "bare_app: LVGL ${BARE_APP_WITH_LVGL}, SDL ${BARE_APP_WITH_SDL}")

# ---- toolkit independent part: no LVGL include path here --------------------
add_library(bare_app_core STATIC
	src/core/bus.c
	src/core/input.c
	src/core/joystick.c
	src/core/log.c
	src/core/mainloop.c
	src/core/msgq.c
	src/core/uevent.c
	src/core/util.c
	src/hal/platform.c
	src/drivers/blit.c
	src/drivers/console.c
	src/drivers/display_drm.c
	src/drivers/display_fbdev.c
	src/drivers/display_mem.c
	src/drivers/input_evdev.c
	src/drivers/input_script.c
	src/drivers/shot.c
	src/drivers/sysfs.c
	src/platforms/oga.c
	src/platforms/pc.c
	src/services/services.c
	src/media/testpattern.c
	src/gui/ctx.c
	src/gui/action.c
	src/gui/notify.c
	src/gui/control/control.c
	src/gui/control/ctl_basic.c
	src/gui/control/ctl_display.c
	src/gui/control/ctl_power.c
	src/gui/view/core/view.c
	src/gui/view/headless/headless.c
)
target_include_directories(bare_app_core PUBLIC src)
target_compile_options(bare_app_core PRIVATE ${WARN})
target_link_libraries(bare_app_core PUBLIC Threads::Threads)
if(BARE_APP_WITH_SDL)
	target_sources(bare_app_core PRIVATE src/drivers/sdl.c)
	target_compile_definitions(bare_app_core PUBLIC BARE_APP_WITH_SDL)
	target_link_libraries(bare_app_core PUBLIC PkgConfig::SDL2)
endif()

# ---- LVGL view backend: the only target that sees LVGL ----------------------
if(BARE_APP_WITH_LVGL)
	set(LVGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lvgl CACHE PATH "LVGL source tree")
	set(LVGL_BACKEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/gui/view/lvgl)
	set(LV_BUILD_CONF_PATH ${LVGL_BACKEND_DIR}/lv_conf.h CACHE PATH "" FORCE)
	set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
	set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
	set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
	set(LV_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)
	set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
	# One self-contained binary: Buildroot passes BUILD_SHARED_LIBS=ON, and
	# a liblvgl.so would not be installed (LVGL install rules are off)
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	add_subdirectory(${LVGL_DIR} lvgl EXCLUDE_FROM_ALL)

	add_library(bare_app_view_lvgl STATIC
		src/gui/view/lvgl/backend.c
		src/gui/view/lvgl/port_display.c
		src/gui/view/lvgl/port_input.c
		src/gui/view/lvgl/widget.c
		src/gui/view/lvgl/screens.c
		src/gui/view/lvgl/widgets/statusbar.c
		src/gui/view/lvgl/screens/home.c
		src/gui/view/lvgl/screens/settings.c
		src/gui/view/lvgl/screens/input_test.c
		src/gui/view/lvgl/screens/layers.c
	)
	target_include_directories(bare_app_view_lvgl PRIVATE ${LVGL_BACKEND_DIR})
	target_compile_options(bare_app_view_lvgl PRIVATE ${WARN})
	target_link_libraries(bare_app_view_lvgl PRIVATE bare_app_core lvgl::lvgl)
endif()

# ---- application -------------------------------------------------------------
add_executable(bare_app src/main.c)
target_compile_options(bare_app PRIVATE ${WARN})
# LVGL's (empty here) C++ sources make CMake add libstdc++; --as-needed drops it
target_link_options(bare_app PRIVATE -Wl,--gc-sections -Wl,--as-needed)
if(BARE_APP_WITH_LVGL)
	target_compile_definitions(bare_app PRIVATE BARE_APP_WITH_LVGL)
	target_link_libraries(bare_app PRIVATE bare_app_view_lvgl)
endif()
target_link_libraries(bare_app PRIVATE bare_app_core m)
install(TARGETS bare_app RUNTIME DESTINATION bin)

# ---- host tests ----------------------------------------------------------------
if(BARE_APP_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
	enable_testing()
	add_executable(test_core tests/test_core.c)
	target_compile_options(test_core PRIVATE ${WARN})
	target_link_libraries(test_core PRIVATE bare_app_core m)
	add_test(NAME core COMMAND test_core)
	add_test(NAME headless_smoke
		COMMAND ${CMAKE_COMMAND} -DBARE_APP=$<TARGET_FILE:bare_app>
			-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_smoke.cmake)
endif()
