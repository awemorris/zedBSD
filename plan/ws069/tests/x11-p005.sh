#!/bin/sh
# ws069-p005: OpenGL 1.x's fixed function through GLX on the Venus guest (the zdesktop image, built by
# plan/ws035/tests/build-zdesktop-image.sh, run by plan/ws035/tests/zdesktop-guest.sh start).  Xzed --rootless
# runs under zdesktop; zgears draws three lit gears from display lists (quads and quad strips, flat and smooth
# shading, one light, the depth test and culling) and reads its first frame back: the red, green and blue
# gears must each cover part of the window over black (ZGEARS CHECK).
#  1. gears.png: the gears turning in their Wiseman window (judge the picture; the log is checked), and
#     gears-later.png two seconds on, which must differ (WS068 p006: the window's frames reach the screen).
#  2. The rate (ZGEARS FPS) is logged.
#
#   plan/ws069/tests/x11-p005.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws069-p005}
mkdir -p "$out"
guest() { timeout 200 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='ps -A -o pid,comm | awk "\$2 == \"zdesktop\" || \$2 == \"Xzed\" || \$2 == \"zgears\" || \$2 == \"glxtest\" {print \$1}" | while read p; do kill $p; done; sleep 1'
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

# 1. zdesktop, Xzed rootless, zgears; the picture after a few seconds.
guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0 /tmp/.X11-unix/X0; /bin/zdesktop --timeout=600 --width=1280 --height=800 --glass --log-frames > /tmp/zdesktop.log 2>&1 </dev/null & sleep 4
DISPLAY=:0 /bin/Xzed --rootless > /tmp/xzed.log 2>&1 </dev/null & sleep 6
DISPLAY=:0 /bin/zgears --frames=300 --token=g > /tmp/gears.log 2>&1 </dev/null & sleep 12; echo started' >/dev/null
pointer move 1250 780 sleep 400
check "$out/gears.png" || status=1
sleep 2
check "$out/gears-later.png" || status=1
if python3 -c 'import sys; from PIL import Image, ImageChops; sys.exit(0 if ImageChops.difference(Image.open(sys.argv[1]).convert("RGB"), Image.open(sys.argv[2]).convert("RGB")).getbbox() else 1)' "$out/gears.png" "$out/gears-later.png"; then
	echo "turn: gears-later.png differs ok"
else
	echo "turn: gears-later.png is the same picture"
	status=1
fi

# 2. The run to its end, its check and rate.
guest 'i=0; while ! grep -q ZGEARS.DONE /tmp/gears.log && [ $i -lt 150 ]; do sleep 1; i=$((i+1)); done; cat /tmp/gears.log' > "$out/gears.txt"
cat "$out/gears.txt"
expect_log /tmp/gears.log 'ZGEARS START run=g .* version="1.4 zedBSD'
expect_log /tmp/gears.log 'ZGEARS CHECK run=g .* failures=0 glerror=0x0'
expect_log /tmp/gears.log 'ZGEARS FPS run=g frames=300'
expect_log /tmp/gears.log 'ZGEARS DONE run=g frames=300 failures=0'
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "x11-p005: PASS (and judge gears.png)" || echo "x11-p005: FAIL"
exit $status
