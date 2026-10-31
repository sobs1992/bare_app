# bare_app_doom: Doom for bare_app (GAME_DOOM). GPLv2 like doomgeneric: this
# directory is the only GPL part of the repository, and nothing here is linked
# into bare_app. Built from bare_app's CMakeLists.txt.

set(DOOM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/doomgeneric/doomgeneric)
if(NOT EXISTS ${DOOM_DIR}/doomgeneric.c)
	message(FATAL_ERROR "bare_app: GAME_DOOM needs ports/doom/doomgeneric: "
			    "git submodule update --init ports/doom/doomgeneric")
endif()

# the Makefile's list without a platform file (doomgeneric_xlib.c...)
set(DOOM_FILES am_map d_event d_items d_iwad d_loop d_main d_mode d_net doomdef
	doomgeneric doomstat dstrings dummy f_finale f_wipe g_game hu_lib hu_stuff
	i_cdmus i_endoom i_input i_joystick i_scale i_sound i_system i_timer i_video
	info m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random
	memio p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj
	p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user
	r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things s_sound sha1 sounds
	st_lib st_stuff statdump tables v_video w_checksum w_file w_file_stdc w_main
	w_wad wi_stuff z_zone)
list(TRANSFORM DOOM_FILES PREPEND ${DOOM_DIR}/)
list(TRANSFORM DOOM_FILES APPEND .c)

add_executable(bare_app_doom doom_main.c doom_sound.c ${DOOM_FILES})
# next to bare_app: that is where it looks for the game
set_target_properties(bare_app_doom PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
# 320x200: the game scales into the video layer itself
# FEATURE_SOUND: sound effects through doom_sound.c (ALSA, no SDL)
target_compile_definitions(bare_app_doom PRIVATE NORMALUNIX LINUX _DEFAULT_SOURCE
	DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200 FEATURE_SOUND)
# bare_app's src/ only for the exchange: games/game_ipc.h, core/input.h (headers)
target_include_directories(bare_app_doom PRIVATE ${DOOM_DIR} ${PROJECT_SOURCE_DIR}/src)
# an empty SDL_mixer.h for i_sound.c
target_include_directories(bare_app_doom PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# 1990s code: no warnings, and what newer compilers made errors is accepted
set_source_files_properties(${DOOM_FILES} PROPERTIES COMPILE_OPTIONS "-w;-fpermissive")
set_source_files_properties(doom_main.c doom_sound.c PROPERTIES COMPILE_OPTIONS
	"-Wall;-Wextra;-Wno-unused-parameter")
target_compile_options(bare_app_doom PRIVATE -fno-strict-aliasing)
target_link_options(bare_app_doom PRIVATE -Wl,--gc-sections)
target_link_libraries(bare_app_doom PRIVATE m Threads::Threads)
install(TARGETS bare_app_doom RUNTIME DESTINATION bin)
