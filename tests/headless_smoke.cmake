# Runs bare_app with the headless backend and a scripted gamepad:
# A opens the input test, B goes back, F6 quits.
set(script "${CMAKE_CURRENT_BINARY_DIR}/headless_smoke.in")
file(WRITE ${script} "wait:300 a wait:300 b wait:300 f6\n")
execute_process(
	COMMAND ${BARE_APP} --platform pc-mem --backend headless
	INPUT_FILE ${script}
	ERROR_VARIABLE log
	RESULT_VARIABLE rc
	TIMEOUT 10)
message("${log}")
if(NOT rc EQUAL 0)
	message(FATAL_ERROR "bare_app exited with ${rc}")
endif()
foreach(want "screen 1 \\(depth 2\\)" "screen 0 \\(depth 1\\)" "quit requested" "bye")
	if(NOT log MATCHES "${want}")
		message(FATAL_ERROR "missing in the log: ${want}")
	endif()
endforeach()
