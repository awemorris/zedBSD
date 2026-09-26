#!/bin/sh
# ws035-p072: minimizing windows and moving them between desktops, on the Venus guest (the zdesktop image).
# zwl --glass runs with a red window (a) and a blue one (b) on desktop 1.
#  1. minimized.png: b's minimize button hides it (the red one only).
#  2. wiseview.png: Wiseview shows b's tile paler; a click on it brings b back (restored.png).
#  3. In Wiseview, a's tile dragged onto desktop 2's picture moves a there; desktop 2 shows a only (desk2.png).
#  4. Ctrl+Alt+Shift+Left takes a back to desktop 1, and desktop 1 is shown with both (desk1.png).
#
#   plan/ws035/tests/zdesktop-p072.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p072}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='ps -A -o pid,comm | awk "{ n = \$2; sub(\".*/\", \"\", n) } n == \"zwl\" || n == \"wlshm\" {print \$1}" | while read p; do kill $p; done; sleep 1'
status=0

# Fails the run unless zwl's log has a line matching a pattern.
expect_log() {
	found=$(guest "grep -cE '$1' /tmp/zwl.log" | tail -1)
	if [ "${found:-0}" -gt 0 ] 2>/dev/null; then
		echo "log: $1 ok"
	else
		echo "log: $1 MISSING"
		status=1
	fi
}

# The tile of a client's window in the latest open Wiseview: x y width height.
tile() {
	guest "grep 'WISEVIEW tile client=$1 ' /tmp/zwl.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p'
}

# Opens Wiseview with the swipe from the bottom edge.
wiseview() {
	pointer move 640 796 sleep 300 down sleep 100 move 640 740 sleep 200 move 640 650 sleep 200 move 640 560 sleep 200 up sleep 1500
}

# zwl, then the red window and the blue one (on top, cascaded).
guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zwl --timeout=600 --width=1280 --height=800 --glass > /tmp/zwl.log 2>&1 </dev/null & sleep 4
/bin/wlshm --size=520x340 --color=ffd04040 --frames=20000 --token=a > /tmp/a.log 2>&1 </dev/null & sleep 3
/bin/wlshm --size=520x340 --color=ff4060d0 --frames=20000 --token=b > /tmp/b.log 2>&1 </dev/null & sleep 3; echo started' >/dev/null
set -- $(guest "grep 'ZWL MAP client=1 ' /tmp/zwl.log | tail -1" | sed -n 's/.* surface=\([0-9]*\) x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2 \3/p')
sa=${1:-0}; ax=${2:-0}; ay=${3:-0}
set -- $(guest "grep 'ZWL MAP client=2 ' /tmp/zwl.log | tail -1" | sed -n 's/.* surface=\([0-9]*\) x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2 \3/p')
sb=${1:-0}; bx=${2:-0}; by=${3:-0}
echo "a: surface $sa at $ax,$ay; b: surface $sb at $bx,$by"

# 1. b's minimize button (the third from its title bar's right edge).
pointer move $((bx + 520 - 26 - 2 * 34)) $((by - 30)) sleep 400 down sleep 60 up sleep 800 move 1250 780 sleep 400
check "$out/minimized.png" --expect $((bx + 100)),$((by + 100)),d04040 || status=1
expect_log "GLASS minimize surface=$sb"

# 2. Wiseview: b's tile, paler; a click brings b back.
wiseview
check "$out/wiseview.png" >/dev/null
set -- $(tile 2); tx=${1:-0}; ty=${2:-0}; tw=${3:-0}; th=${4:-0}
pointer move $((tx + tw / 2)) $((ty + th / 2)) sleep 300 down sleep 60 up sleep 1500 move 1250 780 sleep 400
check "$out/restored.png" --expect $((bx + 100)),$((by + 100)),4060d0 || status=1
expect_log "WISEVIEW select surface=$sb"

# 3. Wiseview again: a's tile dragged onto desktop 2's picture.
set -- $(guest "grep 'ZWL GLASS desktops' /tmp/zwl.log | head -1" | sed -n 's/.* x=\([0-9]*\) step=\([0-9]*\) width=\([0-9]*\).*/\1 \2 \3/p')
d2=$((${1:-850} + ${2:-46} + ${3:-40} / 2))
wiseview
set -- $(tile 1); tx=${1:-0}; ty=${2:-0}; tw=${3:-0}; th=${4:-0}
pointer move $((tx + tw / 2)) $((ty + th / 2)) sleep 300 down sleep 100 move $((tx + tw / 2)) $((ty + th / 2 - 60)) sleep 150 move $((d2 + 40)) 120 sleep 150 move $d2 17 sleep 400
check "$out/dragging.png" >/dev/null
pointer up sleep 800
expect_log "WISEVIEW drag surface=$sa"
expect_log "GLASS move-desktop surface=$sa desktop=2 via=wiseview"
pointer move $d2 17 sleep 300 down sleep 60 up sleep 1500
pointer move 640 17 sleep 300 down sleep 60 up sleep 1200 move 1250 780 sleep 400
check "$out/desk2.png" --expect $((ax + 100)),$((ay + 100)),d04040 || status=1
expect_log "GLASS desktop=2 via=wiseview"

# 4. The key takes a back to desktop 1, where both are.
keys '<ctrl-alt-shift-left>'
sleep 1.5
pointer move 1250 780 sleep 400
check "$out/desk1.png" --expect $((ax + 10)),$((ay + 10)),d04040 || status=1
expect_log "GLASS move-desktop surface=$sa desktop=1 via=key"
expect_log "GLASS desktop settled desktop=1 windows=2"
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "zdesktop-p072: PASS (and judge the screens)" || echo "zdesktop-p072: FAIL"
exit $status
