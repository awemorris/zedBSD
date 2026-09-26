#!/bin/sh
# ws035-p064: the pull that follows the pointer, on the Venus guest (the zdesktop image).  A wl_shm window is
# docked (a double click on its title bar); then its title in the system bar is pulled down:
#  1. pulling.png: 70 px down, the window has shrunk part of the way, rounded, with its floating title bar.
#  2. back.png: let go there, it springs back to the docked space (ZWL GLASS pull back).
#  3. Pulled past 140 px it comes off at its own size under the pointer and keeps moving (undock via=pull).
#
#   plan/ws035/tests/zdesktop-p064.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p064}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
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

# zwl and a window, docked.
guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zwl --timeout=600 --width=1280 --height=800 --glass --log-frames > /tmp/zwl.log 2>&1 </dev/null & sleep 4
/bin/wlshm --size=520x340 --color=ffd04040 --frames=20000 --token=r > /tmp/r.log 2>&1 </dev/null & sleep 3; echo started' >/dev/null
set -- $(guest "grep 'ZWL MAP client=1 ' /tmp/zwl.log" | sed -n 's/.* surface=\([0-9]*\) x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2 \3/p')
surface=${1:-0}; wx=${2:-0}; wy=${3:-0}
pointer move $((wx + 150)) $((wy - 30)) sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 2000
expect_log "GLASS dock surface=$surface via=double-click"
title_x=$(guest "grep 'GLASS dock surface=$surface' /tmp/zwl.log | tail -1" | sed -n 's/.* title=\([0-9]*\).*/\1/p')
tx=$((${title_x:-200} + 60))

# 1. Pulled part of the way: shrinking, with its title bar.
pointer move $tx 17 sleep 400 down sleep 200 move $tx 40 sleep 150 move $tx 87 sleep 800
check "$out/pulling.png" --expect 400,300,d04040 || status=1

# 2. Let go: back to the docked space.
pointer up sleep 1500 move 1250 780 sleep 400
check "$out/back.png" --expect 20,780,d04040 --expect 1260,50,d04040 || status=1
expect_log "GLASS pull back surface=$surface"

# 3. Past the threshold: its own size under the pointer, moving on.
pointer move $tx 17 sleep 400 down sleep 200 move $tx 60 sleep 150 move $tx 120 sleep 150 move $((tx + 100)) 220 sleep 150 move $((tx + 200)) 300 sleep 400 up sleep 1500
expect_log "GLASS undock surface=$surface via=pull"
expect_log "GLASS moved surface=$surface "
pointer move 1250 780 sleep 400
check "$out/pulled.png" >/dev/null
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "zdesktop-p064: PASS (and judge the screens)" || echo "zdesktop-p064: FAIL"
exit $status
