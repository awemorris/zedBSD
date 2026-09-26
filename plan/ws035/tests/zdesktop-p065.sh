#!/bin/sh
# ws035-p065: virtual desktops on the Venus guest (the zdesktop image).  zdesktop --glass runs; a red wl_shm window
# maps on desktop 1 and a blue one on desktop 2.
#  1. desk1.png: desktop 1 shows the red window only.
#  2. desk2.png: a click on desktop 2's picture in the bar slides to it: the blue window only.
#  3. swipe.png / back1.png: a swipe from the left edge follows the pointer (both windows in view), and
#     switches back to desktop 1.
#  4. Ctrl+Alt+Right switches to desktop 2 again; a short swipe from the right edge goes back to 2 (no
#     neighbour change past 25 %); Ctrl+Alt+Left to 1.
#
#   plan/ws035/tests/zdesktop-p065.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p065}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='ps -A -o pid,comm | awk "{ n = \$2; sub(\".*/\", \"\", n) } n == \"zdesktop\" || n == \"wlshm\" {print \$1}" | while read p; do kill $p; done; sleep 1'
status=0

# Fails the run unless zdesktop's log has a line matching a pattern.
expect_log() {
	found=$(guest "grep -cE '$1' /tmp/zdesktop.log" | tail -1)
	if [ "${found:-0}" -gt 0 ] 2>/dev/null; then
		echo "log: $1 ok"
	else
		echo "log: $1 MISSING"
		status=1
	fi
}

# zdesktop and the red window on desktop 1.
guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zdesktop --timeout=600 --width=1280 --height=800 --glass --log-frames > /tmp/zdesktop.log 2>&1 </dev/null & sleep 4
/bin/wlshm --size=520x340 --color=ffd04040 --frames=20000 --token=r > /tmp/r.log 2>&1 </dev/null & sleep 3; echo started' >/dev/null
set -- $(guest "grep 'ZWL GLASS desktops' /tmp/zdesktop.log | head -1" | sed -n 's/.* x=\([0-9]*\) step=\([0-9]*\) width=\([0-9]*\).*/\1 \2 \3/p')
dx=${1:-850}; dstep=${2:-46}; dwidth=${3:-40}
desk() { echo $((dx + ($1 - 1) * dstep + dwidth / 2)); }
pointer move 1250 780 sleep 400
check "$out/desk1.png" --expect 640,450,d04040 || status=1

# 2. Desktop 2 from the bar, and the blue window there.
pointer move $(desk 2) 17 sleep 300 down sleep 60 up sleep 800
guest 'export XDG_RUNTIME_DIR=/tmp; /bin/wlshm --size=520x340 --color=ff4060d0 --frames=20000 --token=u > /tmp/u.log 2>&1 </dev/null & sleep 3; echo started' >/dev/null
pointer move 1250 780 sleep 400
check "$out/desk2.png" --expect 640,450,4060d0 || status=1
expect_log 'ZWL GLASS desktop=2 via=bar'

# 3. A swipe from the left edge: both windows in view half way, then desktop 1.
pointer move 4 450 sleep 200 down sleep 100 move 150 450 sleep 80 move 350 450 sleep 80 move 640 450 sleep 400
check "$out/swipe.png" >/dev/null
pointer up sleep 800 move 1250 780 sleep 400
check "$out/back1.png" --expect 640,450,d04040 || status=1
expect_log 'ZWL GLASS desktop swipe'
expect_log 'ZWL GLASS desktop=1 via=swipe'

# 4. The keys, and a short swipe that goes back.
keys '<ctrl-alt-right>'
sleep 1
pointer move 1250 780 sleep 400
check "$out/key2.png" --expect 640,450,4060d0 || status=1
expect_log 'ZWL GLASS desktop=2 via=key'
pointer move 1275 450 sleep 200 down sleep 100 move 1200 450 sleep 80 move 1100 450 sleep 200 up sleep 800
expect_log 'ZWL GLASS desktop=2 via=swipe'
keys '<ctrl-alt-left>'
sleep 1
expect_log 'ZWL GLASS desktop=1 via=key'
expect_log 'ZWL GLASS desktop settled desktop=1 windows=1'
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "zdesktop-p065: PASS (and judge the screens)" || echo "zdesktop-p065: FAIL"
exit $status
