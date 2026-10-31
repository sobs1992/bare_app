# Kconfig for bare_app (vendored Kconfiglib in tools/kconfig, needs python3).
#
#   BARE_APP_DEFCONFIG  defconfig to start from: a path, or a name in configs/
#                       (default pc_defconfig, oga_defconfig when cross-compiling)
#
# <build>/.config is created from the defconfig, and created again whenever
# the defconfig file is newer or another one is chosen; changes made with
# "make menuconfig" live until then. Keep them with "make update-defconfig"
# (writes the defconfig it came from) or "make savedefconfig" (<build>/defconfig).
# Every configure run writes <build>/bare_app_config.h (included into all
# bare_app sources) and sets CONFIG_* variables for this CMake project.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)
set(KCONFIG_TOOLS ${CMAKE_CURRENT_SOURCE_DIR}/tools/kconfig)
set(KCONFIG_DOTCONFIG ${CMAKE_BINARY_DIR}/.config)
set(KCONFIG_HEADER ${CMAKE_BINARY_DIR}/bare_app_config.h)

set(BARE_APP_DEFCONFIG "" CACHE STRING "defconfig: path or name in configs/")
set(_defconfig "${BARE_APP_DEFCONFIG}")
if(NOT _defconfig)
	if(CMAKE_CROSSCOMPILING)
		set(_defconfig oga_defconfig)
	else()
		set(_defconfig pc_defconfig)
	endif()
endif()
if(NOT _defconfig MATCHES "/")
	set(_defconfig ${CMAKE_CURRENT_SOURCE_DIR}/configs/${_defconfig})
endif()
get_filename_component(_defconfig "${_defconfig}" ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT EXISTS "${_defconfig}")
	message(FATAL_ERROR "bare_app: no defconfig ${_defconfig}")
endif()

set(_kenv ${CMAKE_COMMAND} -E env KCONFIG_CONFIG=${KCONFIG_DOTCONFIG}
	srctree=${CMAKE_CURRENT_SOURCE_DIR})

set(_stamp ${CMAKE_BINARY_DIR}/.config.defconfig)
set(_from "")
if(EXISTS ${_stamp})
	file(READ ${_stamp} _from)
endif()
if(NOT EXISTS ${KCONFIG_DOTCONFIG} OR NOT _from STREQUAL _defconfig
   OR "${_defconfig}" IS_NEWER_THAN ${KCONFIG_DOTCONFIG})
	message(STATUS "bare_app: .config from ${_defconfig}")
	execute_process(COMMAND ${_kenv} ${Python3_EXECUTABLE} ${KCONFIG_TOOLS}/defconfig.py
				--kconfig ${KCONFIG_ROOT} ${_defconfig}
			RESULT_VARIABLE _rc OUTPUT_QUIET)
	if(_rc)
		message(FATAL_ERROR "bare_app: defconfig.py failed")
	endif()
	file(WRITE ${_stamp} "${_defconfig}")
endif()

# Also brings an old .config up to date with the Kconfig (new symbols get defaults)
execute_process(COMMAND ${_kenv} ${Python3_EXECUTABLE} ${KCONFIG_TOOLS}/genconfig.py
			--header-path ${KCONFIG_HEADER} --config-out ${KCONFIG_DOTCONFIG} ${KCONFIG_ROOT}
		RESULT_VARIABLE _rc OUTPUT_QUIET)
if(_rc)
	message(FATAL_ERROR "bare_app: genconfig.py failed")
endif()

file(STRINGS ${KCONFIG_DOTCONFIG} _lines REGEX "^CONFIG_")
foreach(_line ${_lines})
	if(_line MATCHES "^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
		set(_val "${CMAKE_MATCH_2}")
		if(_val STREQUAL "y")
			set(_val ON)
		endif()
		set(${CMAKE_MATCH_1} "${_val}")
	endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	${KCONFIG_DOTCONFIG} ${KCONFIG_ROOT} ${_defconfig})

add_custom_target(menuconfig
	COMMAND ${_kenv} ${Python3_EXECUTABLE} ${KCONFIG_TOOLS}/menuconfig.py ${KCONFIG_ROOT}
	USES_TERMINAL
	COMMENT "bare_app configuration (rebuild picks it up)")
add_custom_target(update-defconfig
	COMMAND ${_kenv} ${Python3_EXECUTABLE} ${KCONFIG_TOOLS}/savedefconfig.py
		--kconfig ${KCONFIG_ROOT} --out ${_defconfig}
	COMMENT "configuration saved back to ${_defconfig}")
add_custom_target(savedefconfig
	COMMAND ${_kenv} ${Python3_EXECUTABLE} ${KCONFIG_TOOLS}/savedefconfig.py
		--kconfig ${KCONFIG_ROOT} --out ${CMAKE_BINARY_DIR}/defconfig
	COMMENT "minimal configuration written to ${CMAKE_BINARY_DIR}/defconfig")
