#!/bin/sh
# ws069-p004: GLX on the Venus guest (the zdesktop image, built by plan/ws035/tests/build-zdesktop-image.sh,
# run by plan/ws035/tests/zdesktop-guest.sh start).  Xzed --rootless runs under zwl; glxtest asks Xzed for
# GLX (version 1.4 and the server's strings) and draws egltest's scene through GLX into its X window, which
# is a Wiseman window.
#  1. glx.png: the scene's colours on the screen, and glxtest's own readback (EGLTEST CHECK).
#  2. docked.png: the window docked; libGL makes the pbuffer again at the new size and the scene follows.
#  3. The close button ends glxtest.
#
#   plan/ws069/tests/x11-p004.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws069-p004}
mkdir -p "$out"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]wl|[X]zed|[g]lxtest|[e]gltest|[z]term" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]wl|[X]zed|[g]lxtest|[e]gltest|[z]term" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
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

# 1. zwl, Xzed rootless, glxtest.
guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0 /tmp/.X11-unix/X0; /bin/zwl --timeout=600 --width=1280 --height=800 --glass --log-frames > /tmp/zwl.log 2>&1 </dev/null & sleep 4
DISPLAY=:0 /bin/Xzed --rootless > /tmp/xzed.log 2>&1 </dev/null & sleep 6
DISPLAY=:0 /bin/glxtest --frames=3000 --delay-ms=30 --token=g > /tmp/glx.log 2>&1 </dev/null & sleep 12; echo started' >/dev/null
set -- $(guest "grep 'ZWL MAP client=1 ' /tmp/zwl.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
wx=${1:-0}; wy=${2:-0}
echo "glxtest window at $wx,$wy"
pointer move 1250 780 sleep 400
check "$out/glx.png" $(scene_expect "$wx" "$wy" 640 400) || status=1
guest 'cat /tmp/glx.log' > "$out/glx.txt"
grep -vE "PIXEL.*ok$" "$out/glx.txt"
expect_log /tmp/glx.log 'GLXTEST GLX run=g version=1.4 server_vendor="zedBSD" server_version="1.4"'
expect_log /tmp/glx.log 'GLXTEST START run=g .* direct=1'
expect_log /tmp/glx.log 'EGLTEST CHECK run=g failures=0 glerror=0x0'

# 2. Docked: a new pbuffer at the new size.
pointer move $((wx + 150)) $((wy - 30)) sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 4000
pointer move 1250 780 sleep 500
set -- $(guest "grep 'GLASS dock surface=' /tmp/zwl.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\) w=\([0-9]*\) h=\([0-9]*\).*/\1 \2 \3 \4/p')
echo "docked: ${1:-?} ${2:-?} ${3:-?} ${4:-?}"
if [ $# -eq 4 ]; then
	check "$out/docked.png" $(scene_expect "$1" "$2" "$3" "$4") || status=1
else
	echo "docked: the window's place is not in the log"
	status=1
fi

# 3. The bar's close button ends glxtest (Xzed keeps running).
close=$(guest "grep 'GLASS dock surface=' /tmp/zwl.log | tail -1" | sed -n 's/.* buttons=\([0-9]*\),.*/\1/p')
pointer move ${close:-0} 17 sleep 500 down sleep 60 up sleep 3000
left=$(guest 'ps -A -o args | grep -c "[g]lxtest"' | tail -1)
xzed=$(guest 'ps -A -o args | grep -c "[X]zed"' | tail -1)
echo "after close: glxtest=${left:-?} Xzed=${xzed:-?}"
[ "${left:-1}" = 0 ] || status=1
[ "${xzed:-0}" -ge 1 ] 2>/dev/null || status=1
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "x11-p004: PASS" || echo "x11-p004: FAIL"
exit $status
