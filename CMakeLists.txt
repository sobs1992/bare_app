cmake_minimum_required(VERSION 3.16)
# LVGL's build enables C++ and ASM for its optional parts
project(bare_app C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# What goes into the binary: Kconfig (make menuconfig, configs/*_defconfig)
include(cmake/kconfig.cmake)

if(NOT CONFIG_VIEW_LVGL AND NOT CONFIG_VIEW_HEADLESS)
	message(FATAL_ERROR "bare_app: enable at least one view backend (VIEW_LVGL, VIEW_HEADLESS)")
endif()
if(NOT CONFIG_PLATFORM_OGA AND NOT CONFIG_PLATFORM_PC)
	message(FATAL_ERROR "bare_app: enable at least one platform")
endif()

set(WARN -Wall -Wextra -Wno-unused-parameter)
add_compile_options(-ffunction-sections -fdata-sections)

find_package(Threads REQUIRED)
if(CONFIG_DISPLAY_SDL)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
	if(NOT SDL2_FOUND)
		message(FATAL_ERROR "bare_app: DISPLAY_SDL needs SDL2 (apt install libsdl2-dev) "
				    "or switch it off: make menuconfig")
	endif()
endif()

# ---- toolkit independent part: no LVGL include path here --------------------
set(CORE_SOURCES
	src/core/bus.c
	src/core/input.c
	src/core/joystick.c
	src/core/log.c
	src/core/mainloop.c
	src/core/msgq.c
	src/core/util.c
	src/hal/platform.c
	src/services/config.c
	src/services/services.c
	src/gui/ctx.c
	src/gui/action.c
	src/gui/notify.c
	src/gui/control/control.c
	src/gui/control/ctl_basic.c
	src/gui/control/ctl_display.c
	src/gui/control/ctl_power.c
	src/gui/view/core/i18n.c
	src/gui/view/core/view.c
)
macro(add_if cond)
	if(${cond})
		list(APPEND CORE_SOURCES ${ARGN})
	endif()
endmacro()
add_if(CONFIG_PLATFORM_OGA      src/platforms/oga.c)
add_if(CONFIG_PLATFORM_PC       src/platforms/pc.c)
add_if(CONFIG_DISPLAY_DRM       src/drivers/display_drm.c)
add_if(CONFIG_DISPLAY_FBDEV     src/drivers/display_fbdev.c)
add_if(CONFIG_DISPLAY_MEM       src/drivers/display_mem.c)
add_if(CONFIG_DISPLAY_SDL       src/drivers/sdl.c)
add_if(CONFIG_INPUT_EVDEV       src/drivers/input_evdev.c)
add_if(CONFIG_INPUT_SCRIPT      src/drivers/input_script.c)
add_if(CONFIG_POWER             src/drivers/sysfs.c)
add_if(CONFIG_BACKLIGHT         src/drivers/sysfs.c)
add_if(CONFIG_VIDEO_TESTPATTERN src/media/mailbox.c src/media/testpattern.c)
add_if(CONFIG_SNAPSHOT          src/drivers/shot.c)
add_if(CONFIG_VIEW_HEADLESS     src/gui/view/headless/headless.c)
if(CONFIG_DISPLAY_DRM OR CONFIG_DISPLAY_FBDEV)
	list(APPEND CORE_SOURCES src/drivers/blit.c src/drivers/console.c)
endif()
if(CONFIG_INPUT_HOTPLUG OR CONFIG_POWER_UEVENT OR CONFIG_BUILD_TESTS)
	list(APPEND CORE_SOURCES src/core/uevent.c)
endif()
if(CONFIG_BUILD_TESTS)
	list(APPEND CORE_SOURCES src/media/mailbox.c)	# unit tested even when unused
endif()
list(REMOVE_DUPLICATES CORE_SOURCES)

add_library(bare_app_core STATIC ${CORE_SOURCES})
target_include_directories(bare_app_core PUBLIC src)
# CONFIG_* of the Kconfig in every bare_app source (not in LVGL)
target_compile_options(bare_app_core PUBLIC "SHELL:-include ${KCONFIG_HEADER}")
target_compile_options(bare_app_core PRIVATE ${WARN})
target_link_libraries(bare_app_core PUBLIC Threads::Threads)
if(CONFIG_DISPLAY_SDL)
	target_link_libraries(bare_app_core PUBLIC PkgConfig::SDL2)
endif()

# ---- LVGL view backend: the only target that sees LVGL ----------------------
if(CONFIG_VIEW_LVGL)
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

	set(LVGL_SOURCES
		src/gui/view/lvgl/backend.c
		src/gui/view/lvgl/port_display.c
		src/gui/view/lvgl/port_input.c
		src/gui/view/lvgl/widget.c
		src/gui/view/lvgl/screens.c
		src/gui/view/lvgl/widgets/statusbar.c
		src/gui/view/lvgl/screens/home.c
	)
	if(CONFIG_UI_FONT_CYRILLIC)
		list(APPEND LVGL_SOURCES src/gui/view/lvgl/fonts/ui_font_14.c
					 src/gui/view/lvgl/fonts/ui_font_20.c)
	endif()
	if(CONFIG_SCREEN_SETTINGS)
		list(APPEND LVGL_SOURCES src/gui/view/lvgl/screens/settings.c)
	endif()
	if(CONFIG_SCREEN_INPUT_TEST)
		list(APPEND LVGL_SOURCES src/gui/view/lvgl/screens/input_test.c)
	endif()
	if(CONFIG_SCREEN_LAYERS)
		list(APPEND LVGL_SOURCES src/gui/view/lvgl/screens/layers.c)
	endif()
	add_library(bare_app_view_lvgl STATIC ${LVGL_SOURCES})
	target_include_directories(bare_app_view_lvgl PRIVATE ${LVGL_BACKEND_DIR})
	target_compile_options(bare_app_view_lvgl PRIVATE ${WARN})
	target_link_libraries(bare_app_view_lvgl PRIVATE bare_app_core lvgl::lvgl)
endif()

# ---- application -------------------------------------------------------------
add_executable(bare_app src/main.c)
target_compile_options(bare_app PRIVATE ${WARN})
# LVGL's (empty here) C++ sources make CMake add libstdc++; --as-needed drops it
target_link_options(bare_app PRIVATE -Wl,--gc-sections -Wl,--as-needed)
if(CONFIG_VIEW_LVGL)
	target_link_libraries(bare_app PRIVATE bare_app_view_lvgl)
endif()
target_link_libraries(bare_app PRIVATE bare_app_core m)
install(TARGETS bare_app RUNTIME DESTINATION bin)

# ---- host tests ----------------------------------------------------------------
if(CONFIG_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
	enable_testing()
	add_executable(test_core tests/test_core.c)
	target_compile_options(test_core PRIVATE ${WARN})
	target_link_libraries(test_core PRIVATE bare_app_core m)
	add_test(NAME core COMMAND test_core)
	add_test(NAME headless_smoke
		COMMAND ${CMAKE_COMMAND} -DBARE_APP=$<TARGET_FILE:bare_app>
			-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless_smoke.cmake)
endif()
