#!/bin/sh
# ws035-p063: Wiseview, the overview of the windows, checked on the Venus
# guest (plan/ws035/wiseman-design.md).
#
# zdesktop --glass (with the wallpaper picture when the guest has one) runs at
# 1280x800 with four windows: a (wltest, pale, 420x300), s (wl_shm, dark
# 2b3444, 360x260), b (wltest, pale blue, 380x280) and m (mview, 640x460,
# on top).  The pointer is driven through one QMP connection.
#  1. A short drag up from the bottom edge does not open Wiseview (cancel).
#  2. A longer drag opens it: peek.png on the way, then open.png; zdesktop logs
#     the frames of the way and each tile.
#  3. The pointer over s's tile (hover.png: glow and close button); a click
#     selects s: Wiseview closes and s is on top (selected.png).
#  4. Opened again, a click on empty space closes it.
#  5. Opened again, b's tile's close button closes b.
#
#   plan/ws035/tests/zdesktop-guest.sh start     (the guest must be up)
#   plan/ws035/tests/zdesktop-p063.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p063}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]desktop( |$)|[w]lshm|[w]ltest|[m]view" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]desktop( |$)|[w]lshm|[w]ltest|[m]view" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
status=0

# Fails the run unless the compositor's log has a line matching a pattern.
expect_log() {
	if guest "grep -E '$1' /tmp/zdesktop.log" | grep -q .; then
		echo "log: $1 ok"
	else
		echo "log: $1 MISSING"
		status=1
	fi
}

# Opens Wiseview by a drag up from the bottom edge, and waits for it to settle.
open_wiseview() {
	pointer move 640 796 sleep 300 down sleep 100 move 640 760 sleep 200 move 640 700 sleep 200 move 640 600 sleep 200 up sleep 1500
}

# The tile of a client in the latest open Wiseview: x y width height.
tile() {
	guest "grep 'WISEVIEW tile client=$1 ' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p'
}

guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/zdesktop/wallpaper.ppm ] && picture=--wallpaper=/usr/share/zdesktop/wallpaper.ppm
/bin/zdesktop --timeout=600 --width=1280 --height=800 --glass $picture --log-frames > /tmp/zdesktop.log 2>&1 </dev/null & sleep 5
/bin/wltest --windowed --size=420x300 --color=f4f7fc --frames=3600 --delay-ms=100 --token=a > /tmp/a.log 2>&1 </dev/null & sleep 2
/bin/wlshm --size=360x260 --color=ff2b3444 --frames=9000 --token=s > /tmp/s.log 2>&1 </dev/null & sleep 2
/bin/wltest --windowed --size=380x280 --color=dfe9f7 --frames=3600 --delay-ms=100 --token=b > /tmp/b.log 2>&1 </dev/null & sleep 2
/bin/mview --windowed --size=640x460 --timeout-s=500 --token=m > /tmp/m.log 2>&1 </dev/null & sleep 10; echo started' >/dev/null

# 1. A short drag does not open it.
pointer move 640 796 sleep 300 down sleep 100 move 640 770 sleep 300 up sleep 800
expect_log "WISEVIEW cancel from=0\.[0-2]"

# 2. A longer drag opens it, drawn on the way.
pointer move 640 796 sleep 300 down sleep 100 move 640 740 sleep 300 move 640 700 sleep 700
check "$out/peek.png" >/dev/null
pointer move 640 600 sleep 300 up sleep 1500 move 1200 796 sleep 500
check "$out/open.png" >/dev/null || status=1
expect_log "WISEVIEW opening from="
expect_log "WISEVIEW open windows=4"
frames=$(guest "grep -c 'WISEVIEW frame progress=0\.' /tmp/zdesktop.log")
echo "frames on the way: $frames"
[ "$frames" -ge 2 ] || status=1

# 3. s's tile: hover, then select.
set -- $(tile 2); sx=$1; sy=$2; sw=$3; sh=$4
pointer move $((sx + sw / 2)) $((sy + sh / 2)) sleep 800
check "$out/hover.png" >/dev/null
pointer down sleep 80 up sleep 1500 move 1200 796 sleep 500
expect_log "WISEVIEW select surface="
expect_log "WISEVIEW closed"
set -- $(guest "grep 'ZWL MAP client=2 ' /tmp/zdesktop.log" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
check "$out/selected.png" --expect $(($1 + 180)),$(($2 + 130)),2b3444 || status=1

# 4. Opened again, a click on empty space closes it.
open_wiseview
pointer move 20 420 sleep 300 down sleep 80 up sleep 1200
expect_log "WISEVIEW close$"

# 5. Opened again, b's close button closes b.
open_wiseview
set -- $(tile 3); bx=$1; by=$2; bw=$3
pointer move $((bx + bw / 2)) $((by + 40)) sleep 500 move $((bx + bw - 14)) $((by + 14)) sleep 500 down sleep 80 up sleep 2000
expect_log "WISEVIEW close-window surface="
left=$(guest 'ps -A -o args | grep -c "[w]ltest.*token=b"')
echo "b still running: $left"
[ "$left" = 0 ] || status=1
pointer move 20 420 sleep 300 down sleep 80 up sleep 1200

# Nothing failed.
guest 'grep -E "ERROR|FAILED" /tmp/zdesktop.log /tmp/a.log /tmp/s.log /tmp/b.log /tmp/m.log' | tee "$out/errors.txt"
[ -s "$out/errors.txt" ] && status=1
guest 'grep -E "WISEVIEW (opening|open|tile|cancel|close|select|closed)" /tmp/zdesktop.log' > "$out/log.txt"
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "p063: PASS" || echo "p063: FAIL"
exit $status
