#!/bin/sh
# Build bare_app for the ODROID-GO Advance and drive it over the serial console
# (scripts/oga-serial.py of the SDK). The SDK is two levels up unless SDK= is set.
#
#   tools/oga.sh build          cross-compile into build-oga/
#   tools/oga.sh run [ARGS]     build, copy to /tmp/bare_app if changed, restart in background
#   tools/oga.sh stop           stop it and show the end of the log
#   tools/oga.sh log            whole log (/tmp/bare_app.log)
#   tools/oga.sh shot FILE.png  screenshot: all layers from the running app (SIGUSR1),
#                               else /dev/fb0
#   tools/oga.sh key NAMES...   press buttons (up down left right a b x y l r f1..f6)
set -e

APP=$(cd "$(dirname "$0")/.." && pwd)
SDK=${SDK:-$(cd "$APP/../.." && pwd)}
HOST=$SDK/output/oga/buildroot/host
SERIAL="$SDK/scripts/oga-serial.py"
OUT=$APP/build-oga
REMOTE=/tmp/bare_app

build() {
	[ -f "$HOST/share/buildroot/toolchainfile.cmake" ] ||
		{ echo "no OGA toolchain in $HOST: run 'make FW=oga rootfs' in the SDK" >&2; exit 1; }
	cmake -S "$APP" -B "$OUT" -DCMAKE_BUILD_TYPE=Release \
	      -DCMAKE_TOOLCHAIN_FILE="$HOST/share/buildroot/toolchainfile.cmake" > /dev/null
	cmake --build "$OUT" -j"$(nproc)"
	"$HOST/bin/aarch64-buildroot-linux-gnu-strip" -o "$OUT/bare_app.stripped" "$OUT/bare_app"
}

push() {
	local_md5=$(md5sum < "$OUT/bare_app.stripped" | cut -d' ' -f1)
	remote_md5=$("$SERIAL" sh "md5sum $REMOTE 2>/dev/null" | cut -d' ' -f1) || true
	if [ "$local_md5" = "$remote_md5" ]; then
		echo "board already has this build"
	else
		echo "copying $(stat -c %s "$OUT/bare_app.stripped") bytes over the console..."
		"$SERIAL" sh "killall bare_app 2>/dev/null; true" > /dev/null
		"$SERIAL" push "$OUT/bare_app.stripped" $REMOTE
	fi
}

cmd=${1:-run}
[ $# -gt 0 ] && shift
case $cmd in
build) build ;;
run)
	build
	push
	"$SERIAL" sh "killall bare_app 2>/dev/null; sleep 0.3; ($REMOTE $* > /tmp/bare_app.log 2>&1 &); sleep 1; cat /tmp/bare_app.log"
	;;
stop) "$SERIAL" sh "killall bare_app 2>/dev/null; sleep 0.5; tail -n 20 /tmp/bare_app.log" ;;
log)  "$SERIAL" sh "cat /tmp/bare_app.log" ;;
shot)
	out=${1:?file.png}
	if "$SERIAL" sh 'kill -USR1 $(pidof bare_app) 2>/dev/null && sleep 0.5' > /dev/null; then
		"$SERIAL" pull /tmp/bare_app.ppm "$out.ppm"
		python3 "$APP/tools/ppm2png.py" "$out.ppm" "$out" && rm -f "$out.ppm"
	else
		"$SERIAL" shot "$out"
	fi
	;;
key)  "$SERIAL" key --delay 0.3 "$@" ;;
*)    sed -n '2,13p' "$0"; exit 1 ;;
esac
