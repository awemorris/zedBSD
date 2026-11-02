#!/bin/sh
# Builds plan/ws345/tests/resize-main.c (kl_window_on_resize in the system's modal size loop) for x86_64
# Windows with the browser's Windows build and runs it under Wine in Xvfb: three drags of the frame's
# right edge with xdotool (the frame's resize band, the system's modal size loop), one for each round of
# the program.  Last line: resize-wine: PASS or resize-wine: FAIL.
#
#   sh plan/ws345/tests/resize-wine.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64);
# WINEPREFIX=<dir> the Wine prefix (default build/tmp/ws345-resize-wine, made when missing).  The Wine
# commands run under flock /tmp/zedbsd-wine.lock.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
WINEPREFIX=${WINEPREFIX:-$(pwd)/build/tmp/ws345-resize-wine}
export WINEPREFIX
build=build/tmp/ws345-resize-win64
sources=build/tmp/ws334-win32-sources
lock=/tmp/zedbsd-wine.lock
mkdir -p "$build" "$sources"

# The program, warning 0.
log=$build/make.log
if ! timeout 3000 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 -j8 \
    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" \
    test-program TEST_PROGRAM=resize-test TEST_PROGRAM_SOURCES=plan/ws345/tests/resize-main.c > "$log" 2>&1 ||
    grep -q 'warning:' "$log"; then
	echo "resize-wine: the build failed or warned (log $log)"
	echo "resize-wine: FAIL"
	exit 1
fi
program=$build/tests/resize-test.exe

# The prefix.
if [ ! -f "$WINEPREFIX/system.reg" ]; then
	WINEDEBUG=-all timeout 600 flock "$lock" xvfb-run -a wineboot -i > "$build/wineboot.log" 2>&1
fi

# The run in an Xvfb of its own: the program, and a drag of the right edge for each of its rounds.
run=$build/run.log
: > "$run"
cat > "$build/drive.sh" <<'DRIVE'
#!/bin/sh
# $1 the program, $2 its output.
wine "$1" > "$2" 2>&1 &
pid=$!
wait_line() {
	n=0
	while ! grep -q "$1" "$2"; do
		n=$((n + 1))
		[ "$n" -gt 300 ] && return 1
		sleep 0.1
	done
}
wait_line '^resize-test: ready' "$2" || { kill "$pid"; exit 1; }
set -- "$1" "$2" $(sed -n 's/^resize-test: ready //p' "$2")
x=$3 y=$4 width=$5 height=$6
for round in 1 2 3; do
	sleep 0.5
	edge=$((x + width - 3))
	xdotool mousemove "$edge" $((y + height / 2))
	sleep 0.3
	xdotool mousedown 1
	step=1
	while [ "$step" -le 8 ]; do
		sleep 0.15
		xdotool mousemove $((edge + step * 15)) $((y + height / 2))
		step=$((step + 1))
	done
	sleep 0.3
	xdotool mouseup 1
	width=$((width + 120))
	wait_line "^resize-test: round $round done" "$2" || break
done
wait "$pid"
DRIVE
WINEDEBUG=-all timeout 180 flock "$lock" xvfb-run -a -s '-screen 0 1280x800x24' sh "$build/drive.sh" "$program" "$run"
status=$?
sync
grep '^resize-test:' "$run"
if [ "$status" -ne 0 ] || ! grep -q '^resize-test: PASS' "$run"; then
	echo "resize-wine: FAIL (exit $status, log $run)"
	exit 1
fi
echo "resize-wine: PASS"
