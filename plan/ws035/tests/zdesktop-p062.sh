#!/bin/sh
# ws035-p062: docking a window's title bar to the system bar, checked on the
# Venus guest.
#
# zwl --glass (with the wallpaper picture when the guest has one) runs at
# 1280x800.  Window a is a pale 420x300 wltest window, window m the model
# viewer (mview --windowed, 640x460, background 333333) over it.  The pointer
# is driven through one QMP connection (qmp-pointer.py), so a double click
# fits in zwl's 400 ms.
#  1. floating.png: both windows floating.
#  2. A double click on m's title bar docks it: zwl logs the dock, m is told
#     1280x762, the animation draws frames, and m fills the output under the
#     bar (docked.png, and docked-hover.png with the pointer on restore).
#  3. A double click on the title in the bar brings m back where it was
#     (told 640x460).
#  4. m's title bar dragged into the bar: the hint shows (hint.png), and
#     letting go docks m.
#  5. The title in the bar pulled down: m comes back under the pointer and
#     keeps moving; letting go leaves it there (pulled.png).
#  6. m's maximize button docks it; the bar's restore button brings it back.
#
#   plan/ws035/tests/zdesktop-guest.sh start     (the guest must be up)
#   plan/ws035/tests/zdesktop-p062.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p062}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]wl|[w]lshm|[w]ltest|[m]view" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]wl|[w]lshm|[w]ltest|[m]view" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
status=0

# Counts log lines matching a pattern.
count() {
	guest "grep -cE '$1' /tmp/zwl.log"
}

# Fails the run unless the compositor's log has a line matching a pattern.
expect_log() {
	if guest "grep -E '$1' /tmp/zwl.log" | grep -q .; then
		echo "log: $1 ok"
	else
		echo "log: $1 MISSING"
		status=1
	fi
}

guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/zdesktop/wallpaper.ppm ] && picture=--wallpaper=/usr/share/zdesktop/wallpaper.ppm
/bin/zwl --timeout=600 --width=1280 --height=800 --glass $picture --log-frames > /tmp/zwl.log 2>&1 </dev/null & sleep 5
/bin/wltest --windowed --size=420x300 --color=f4f7fc --frames=3600 --delay-ms=100 --token=a > /tmp/a.log 2>&1 </dev/null & sleep 2
/bin/mview --windowed --size=640x460 --timeout-s=500 --token=m > /tmp/m.log 2>&1 </dev/null & sleep 10; echo started' >/dev/null
set -- $(guest "grep 'ZWL MAP client=2 ' /tmp/zwl.log" | sed -n 's/.* surface=\([0-9]*\) x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2 \3/p')
surface=$1; mx=$2; my=$3
title_y=$((my - 8 - 22))
echo "m: surface $surface at $mx,$my"

# 1. Floating.
pointer move 1200 780 sleep 500
check "$out/floating.png" --expect $((mx + 20)),$((my + 200)),333333 >/dev/null || status=1

# 2. A double click on m's title bar docks it.
pointer move $((mx + 150)) $title_y sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 100
check "$out/docking.png" >/dev/null
sleep 3
expect_log "GLASS dock surface=$surface via=double-click"
expect_log "CONFIGURE client=2 surface=$surface serial=[0-9]* width=1280 height=762"
frames=$(count "GLASS anim surface=$surface docking=1")
echo "animation frames while docking: $frames"
[ "$frames" -ge 1 ] || status=1
set -- $(guest "grep 'GLASS dock surface=$surface via=double-click' /tmp/zwl.log | tail -1" | sed -n 's/.* buttons=\([0-9]*\),\([0-9]*\),\([0-9]*\) title=\([0-9]*\).*/\1 \2 \3 \4/p')
close=$1; restore=$2; minimize=$3; title_x=$4
pointer move 1200 780 sleep 500
check "$out/docked.png" --expect 20,780,333333 --expect 1260,50,333333 --expect 640,700,333333 || status=1
pointer move "$restore" 17 sleep 800
check "$out/docked-hover.png" >/dev/null

# 3. A double click on the title in the bar brings it back.
pointer move $((title_x + 60)) 17 sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 100
sleep 3
expect_log "GLASS undock surface=$surface via=double-click x=$mx y=$my"
expect_log "CONFIGURE client=2 surface=$surface serial=[0-9]* width=640 height=460"
pointer move 1200 780 sleep 500
check "$out/undocked.png" --expect $((mx + 20)),$((my + 200)),333333 || status=1

# 4. The title bar dragged into the system bar docks it.
pointer move $((mx + 150)) $title_y sleep 500 down sleep 200 \
    move $((mx + 150)) $((title_y - 60)) sleep 200 \
    move $((mx + 150)) $((title_y - 120)) sleep 200 \
    move $((mx + 150)) 12 sleep 1200
check "$out/hint.png" >/dev/null
pointer up sleep 3000
expect_log "GLASS dock surface=$surface via=drag"

# 5. The title in the bar pulled down: m comes off under the pointer and keeps moving.
pointer move $((title_x + 60)) 17 sleep 500 down sleep 200 \
    move $((title_x + 60)) 30 sleep 200 \
    move $((title_x + 60)) 60 sleep 400 \
    move $((title_x + 160)) 160 sleep 400 \
    move $((title_x + 260)) 260 sleep 400 up sleep 2000
expect_log "GLASS undock surface=$surface via=pull"
set -- $(guest "grep 'GLASS moved surface=$surface ' /tmp/zwl.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
px=${1:-0}; py=${2:-0}
echo "m after the pull: $px,$py"
[ "$py" -gt 0 ] || status=1
pointer move 1200 780 sleep 500
check "$out/pulled.png" --expect $((px + 20)),$((py + 200)),333333 || status=1

# 6. The maximize button docks it; the bar's restore button brings it back.
pointer move $((px + 640 - 26 - 34)) $((py - 8 - 22)) sleep 500 down sleep 80 up sleep 3000
expect_log "GLASS dock surface=$surface via=button"
pointer move "$restore" 17 sleep 500 down sleep 80 up sleep 3000
expect_log "GLASS undock surface=$surface via=button x=$px y=$py"
pointer move 1200 780 sleep 500
check "$out/restored.png" --expect $((px + 20)),$((py + 200)),333333 || status=1

# Nothing failed.
guest 'grep -E "ERROR|FAILED" /tmp/zwl.log /tmp/a.log /tmp/m.log' | tee "$out/errors.txt"
[ -s "$out/errors.txt" ] && status=1
guest 'grep -E "GLASS (dock|undock|moved)|CONFIGURE client=2" /tmp/zwl.log' > "$out/log.txt"
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "p062: PASS" || echo "p062: FAIL"
exit $status
