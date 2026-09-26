#!/bin/sh
# ws069-p002: Xzed's rootful Wayland backend on the Venus guest (the zdesktop image).
#  1. xzed.png: Xzed --wayland runs zterm (an X client) in a zwl --glass window; its shell's prompt shows.
#  2. typed.png: a click on the window and `echo X11-OK; uname -a` typed through QMP: the output shows.
#
#   plan/ws069/tests/x11-p002.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws069-p002}
mkdir -p "$out"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]wl|[X]zed|[z]term|[w]lshm|[m]view|[e]gltest|[z]desktop-terminal" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]wl|[X]zed|[z]term" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
status=0

guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zwl --timeout=600 --width=1280 --height=800 --glass --log-frames > /tmp/zwl.log 2>&1 </dev/null & sleep 4
DISPLAY=:0 /bin/Xzed --wayland --size 800x500 -- /bin/zterm > /tmp/xzed.log 2>&1 </dev/null & sleep 8; echo started' >/dev/null
guest 'cat /tmp/xzed.log; grep "ZWL MAP" /tmp/zwl.log' | tee "$out/start.txt"
set -- $(guest "grep 'ZWL MAP client=1 ' /tmp/zwl.log" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
wx=${1:-0}; wy=${2:-0}
[ "$wx" -gt 0 ] || status=1
pointer move 1250 780 sleep 500
check "$out/xzed.png" >/dev/null

# Typing into the X window.
pointer move $((wx + 400)) $((wy + 250)) sleep 300 down sleep 60 up sleep 500
keys 'echo X11-OK; uname -a\n'
sleep 3
pointer move 1250 780 sleep 500
check "$out/typed.png" >/dev/null
guest 'grep -E "FAILED|ERROR" /tmp/zwl.log; cat /tmp/xzed.log' | tee "$out/logs.txt"
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "x11-p002: done (judge the screens)" || echo "x11-p002: FAIL"
exit $status
