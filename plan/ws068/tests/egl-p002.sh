#!/bin/sh
# ws068-p002: EGL and the clear of OpenGL ES on the Venus guest (the zdesktop image, built by
# plan/ws035/tests/build-zdesktop-image.sh, run by plan/ws035/tests/zdesktop-guest.sh start).
#  1. wayland.png: egltest in a zwl --glass window clears to 3a78c8; the window's middle has that colour.
#  2. resized.png: the window docked (a double click on its title bar): EGL makes the swapchain again at
#     the new size and the colour fills it.
#  3. display.png: without zwl, egltest --platform=display clears the whole screen to c8503a.
#
#   plan/ws068/tests/egl-p002.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws068-p002}
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

# 1. In a zwl window.
guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zwl --timeout=600 --width=1280 --height=800 --glass --log-frames > /tmp/zwl.log 2>&1 </dev/null & sleep 4
/bin/egltest --display=/tmp/wayland-0 --size=640x400 --color=3a78c8 --frames=4000 --delay-ms=30 --token=w > /tmp/egl-w.log 2>&1 </dev/null & sleep 6; echo started' >/dev/null
guest 'cat /tmp/egl-w.log' | tee "$out/wayland.txt"
set -- $(guest "grep 'ZWL MAP client=1 ' /tmp/zwl.log" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
wx=${1:-0}; wy=${2:-0}
pointer move 1250 780 sleep 400
check "$out/wayland.png" --expect $((wx + 320)),$((wy + 200)),3a78c8 || status=1
expect_log /tmp/egl-w.log 'EGLTEST START run=w platform=wayland egl=1.5'

# 2. Docked: the swapchain is made again at the new size.
pointer move $((wx + 150)) $((wy - 30)) sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 3000
pointer move 1250 780 sleep 500
check "$out/resized.png" --expect 40,760,3a78c8 --expect 1240,60,3a78c8 || status=1

# The bar's close button ends egltest normally (its loop sees the compositor's close).
close=$(guest "grep 'GLASS dock surface=' /tmp/zwl.log | tail -1" | sed -n 's/.* buttons=\([0-9]*\),.*/\1/p')
pointer move ${close:-0} 17 sleep 500 down sleep 60 up sleep 2000
expect_log /tmp/egl-w.log 'EGLTEST DONE run=w'
guest "$stop_all" >/dev/null

# 3. The whole screen, without a compositor.
guest '/bin/egltest --platform=display --color=c8503a --frames=300 --delay-ms=30 --token=d > /tmp/egl-d.log 2>&1 </dev/null & sleep 5; echo started' >/dev/null
check "$out/display.png" --expect 640,400,c8503a --expect 20,20,c8503a || status=1
guest 'i=0; while ! grep -q EGLTEST.DONE /tmp/egl-d.log && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done; cat /tmp/egl-d.log' | tee "$out/display.txt"
expect_log /tmp/egl-d.log 'EGLTEST START run=d platform=display'
expect_log /tmp/egl-d.log 'EGLTEST DONE run=d frames=300'

guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "egl-p002: PASS" || echo "egl-p002: FAIL"
exit $status
