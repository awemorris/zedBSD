#!/bin/sh
# ws069-p003: Xzed rootless on the Venus guest (the zdesktop image).
#  1. rootless.png: Xzed --rootless runs zterm; zterm's X window is a Wiseman window titled "zterm".
#  2. typed.png: a click and `echo ROOTLESS-OK; uname -a` typed: the output shows.
#  3. docked.png: a double click on the title bar docks it; the X window takes the new size and zterm
#     draws its grid again at it.
#  4. The close button ends zterm (Xzed keeps running).
#
#   plan/ws069/tests/x11-p003.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws069-p003}
mkdir -p "$out"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]desktop( |$)|[X]zed|[z]term" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]desktop( |$)|[X]zed|[z]term" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
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

guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zdesktop --timeout=600 --width=1280 --height=800 --glass --log-frames > /tmp/zdesktop.log 2>&1 </dev/null & sleep 4
DISPLAY=:0 /bin/Xzed --rootless --size 1000x620 -- /bin/zterm > /tmp/xzed.log 2>&1 </dev/null & sleep 8; echo started' >/dev/null
guest 'cat /tmp/xzed.log; grep -E "ZWL (MAP|TITLE)" /tmp/zdesktop.log' | tee "$out/start.txt"
set -- $(guest "grep 'ZWL MAP client=1 ' /tmp/zdesktop.log | tail -1" | sed -n 's/.* surface=\([0-9]*\) x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2 \3/p')
surface=${1:-0}; wx=${2:-0}; wy=${3:-0}
echo "zterm window: surface $surface at $wx,$wy"
pointer move 1250 780 sleep 500
check "$out/rootless.png" >/dev/null

# 2. Typing.
pointer move $((wx + 300)) $((wy + 200)) sleep 300 down sleep 60 up sleep 500
keys 'echo ROOTLESS-OK; uname -a\n'
sleep 3
pointer move 1250 780 sleep 500
check "$out/typed.png" >/dev/null

# 3. Docked: the X window takes the new size.
pointer move $((wx + 150)) $((wy - 30)) sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 4000
keys 'ls /\n'
sleep 3
pointer move 1250 780 sleep 500
check "$out/docked.png" >/dev/null
expect_log "GLASS dock surface=$surface via=double-click"

# 4. The bar's close button ends zterm.
close=$(guest "grep 'GLASS dock surface=' /tmp/zdesktop.log | tail -1" | sed -n 's/.* buttons=\([0-9]*\),.*/\1/p')
pointer move ${close:-0} 17 sleep 500 down sleep 60 up sleep 3000
running=$(guest 'ps -A -o args | grep -c "[z]term"' | tail -1)
echo "zterm processes after close: $running"
[ "${running:-1}" = 0 ] || status=1
xzed=$(guest 'ps -A -o args | grep -c "[X]zed"' | tail -1)
echo "Xzed processes after close: $xzed"
guest 'grep -E "FAILED|ERROR" /tmp/zdesktop.log; cat /tmp/xzed.log' | tee "$out/logs.txt"
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "x11-p003: PASS (and judge the screens)" || echo "x11-p003: FAIL"
exit $status
