# Runs bare_app (pc-mem, LVGL) with tests/fake_game.c as the only game of
# a game.ini: starts it from the menu (the third game tile, after
# Half-Life and Doom of pc_defconfig), presses A in it, minimises it (F5)
# and brings it back, ends it with B (status 0, back to the menu), starts
# it again and ends it with X (status 3, the screen says so), quits.
set(script "${CMAKE_CURRENT_BINARY_DIR}/game_smoke.in")
file(WRITE ${script} "wait:500 right right a wait:700 a wait:300 f5 wait:500 f5 wait:500 b wait:700 "
		     "right right a wait:700 x wait:700 b wait:300 f6:2300\n")
set(ENV{BARE_APP_NATIVE_DIRS} "${GAMES_DIR}")
set(ENV{BARE_APP_NATIVE_DATA} "${CMAKE_CURRENT_BINARY_DIR}/game_smoke_data")
set(ENV{BARE_APP_WINE_GAMES} "${CMAKE_CURRENT_BINARY_DIR}/game_smoke_none")
execute_process(
	COMMAND ${BARE_APP} --platform pc-mem
	INPUT_FILE ${script}
	ERROR_VARIABLE log
	RESULT_VARIABLE rc
	TIMEOUT 20)
message("${log}")
if(NOT rc EQUAL 0)
	message(FATAL_ERROR "bare_app exited with ${rc}")
endif()
foreach(want "fake: started, frames 480x320" "view: game native, entry 0" "fake: button 4 down"
	    "game: paused" "game: resumed" "fake: button 5 down" "exit status 0"
	    "native: [1-9][0-9]* frames shown" "fake: button 6 down" "exit status 3" "bye")
	if(NOT log MATCHES "${want}")
		message(FATAL_ERROR "missing in the log: ${want}")
	endif()
endforeach()
