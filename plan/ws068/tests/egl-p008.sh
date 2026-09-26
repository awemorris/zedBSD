#!/bin/sh
# ws068-p008: OpenGL ES 2 drawing on the Venus guest (the zdesktop image, built by
# plan/ws035/tests/build-zdesktop-image.sh, run by plan/ws035/tests/zdesktop-guest.sh start).
# egltest --scene=draw draws scene.c's shapes: a strip from a buffer object with byte colours,
# a textured fan from client arrays, a half-alpha blend, a depth test with glDrawElements, and
# culling.  Each run is checked twice: egltest reads its first frame back (glReadPixels, the
# EGLTEST CHECK line) and the screen is photographed and read.
#  1. display.png: without zwl, the whole screen.
#  2. wayland.png: in a zwl --glass window of 640x400.
#  3. resized.png: the window docked; the scene follows the new size.
#
#   plan/ws068/tests/egl-p008.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws068-p008}
mkdir -p "$out"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]wl|[w]lshm|[w]ltest|[m]view|[e]gltest|[z]desktop-terminal" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]wl|[w]lshm|[w]ltest|[m]view|[e]gltest|[z]desktop-terminal" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
status=0

# Fails the run unless a log has a line matching a pattern.
expect_log() {
	found=$(guest "grep -cE '$2' $1" | tail -1)
	if [ "${found:-0}" -gt 0 ] 2>/dev/null; then
		echo "log: $2 ok"
	else
		echo "log: $2 MISSING"
		status=1
	fi
}

# The scene's colours on the screen, for a window at X,Y of W x H (screen coordinates, from the top).
scene_expect() {
	x=$1; y=$2; w=$3; h=$4
	echo "--expect $((x + w / 4)),$((y + h / 4)),ff8000" \
	     "--expect $((x + w * 65 / 100)),$((y + h * 35 / 100)),ff0000" \
	     "--expect $((x + w * 85 / 100)),$((y + h * 35 / 100)),00ff00" \
	     "--expect $((x + w * 65 / 100)),$((y + h * 15 / 100)),0000ff" \
	     "--expect $((x + w * 85 / 100)),$((y + h * 15 / 100)),ffffff" \
	     "--expect $((x + w * 15 / 100)),$((y + h * 3 / 4)),0000ff" \
	     "--expect $((x + w * 3 / 4)),$((y + h * 3 / 4)),00ff00" \
	     "--expect $((x + w * 58 / 100)),$((y + h * 88 / 100)),ff0000" \
	     "--expect $((x + w * 92 / 100)),$((y + h * 675 / 1000)),ff0000" \
	     "--expect $((x + w / 2)),$((y + h / 2)),202020"
}

# 1. The whole screen, without a compositor.
guest "$stop_all" >/dev/null
guest '/bin/egltest --platform=display --scene=draw --frames=200 --delay-ms=30 --token=d > /tmp/egl-d.log 2>&1 </dev/null & sleep 5; echo started' >/dev/null
check "$out/display.png" $(scene_expect 0 0 1280 800) || status=1
guest 'i=0; while ! grep -q EGLTEST.DONE /tmp/egl-d.log && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done; cat /tmp/egl-d.log' > "$out/display.txt"
cat "$out/display.txt"
expect_log /tmp/egl-d.log 'EGLTEST CHECK run=d failures=0 glerror=0x0'
expect_log /tmp/egl-d.log 'EGLTEST DONE run=d frames=200 glerror=0x0 failures=0'

# 2. In a zwl window.
guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zwl --timeout=600 --width=1280 --height=800 --glass --log-frames > /tmp/zwl.log 2>&1 </dev/null & sleep 4
/bin/egltest --display=/tmp/wayland-0 --size=640x400 --scene=draw --frames=4000 --delay-ms=30 --token=w > /tmp/egl-w.log 2>&1 </dev/null & sleep 6; echo started' >/dev/null
set -- $(guest "grep 'ZWL MAP client=1 ' /tmp/zwl.log" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
wx=${1:-0}; wy=${2:-0}
pointer move 1250 780 sleep 400
check "$out/wayland.png" $(scene_expect "$wx" "$wy" 640 400) || status=1
guest 'cat /tmp/egl-w.log' > "$out/wayland.txt"
cat "$out/wayland.txt"
expect_log /tmp/egl-w.log 'EGLTEST CHECK run=w failures=0 glerror=0x0'

# 3. Docked: the swapchain and the depth buffer are made again at the new size and the scene follows.
pointer move $((wx + 150)) $((wy - 30)) sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 3000
pointer move 1250 780 sleep 500
set -- $(guest "grep 'GLASS dock surface=' /tmp/zwl.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\) w=\([0-9]*\) h=\([0-9]*\).*/\1 \2 \3 \4/p')
echo "docked: ${1:-?} ${2:-?} ${3:-?} ${4:-?}"
if [ $# -eq 4 ]; then
	check "$out/resized.png" $(scene_expect "$1" "$2" "$3" "$4") || status=1
else
	echo "docked: the window's place is not in the log"
	status=1
fi

# The bar's close button ends egltest normally.
close=$(guest "grep 'GLASS dock surface=' /tmp/zwl.log | tail -1" | sed -n 's/.* buttons=\([0-9]*\),.*/\1/p')
pointer move ${close:-0} 17 sleep 500 down sleep 60 up sleep 2000
expect_log /tmp/egl-w.log 'EGLTEST DONE run=w .*failures=0'
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "egl-p008: PASS" || echo "egl-p008: FAIL"
exit $status
