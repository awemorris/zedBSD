#!/bin/sh
# ws035-p068: zdesktop-terminal in zdesktop --glass on the Venus guest.
#
# zdesktop runs at 1280x800 with the wallpaper; the terminal opens a window with
# /bin/sh on a pseudo-terminal.  Keys typed through QMP (qmp-keys.py) run
# commands whose output must show in the window:
#  1. prompt.png: the window with the shell's prompt (the window's colour at
#     its centre, the terminal's background 1d2230).
#  2. output.png: after `echo ZTERM-OK; ls /` (the log shows no failure).
#  3. docked.png: a double click on the title bar docks the window; the
#     terminal is told the new size (ZTERM RESIZE in its log).
#  4. `exit` ends the shell and the terminal (ZTERM DONE reason=shell-exited).
#
#   plan/ws035/tests/zdesktop-guest.sh start     (the guest must be up)
#   plan/ws035/tests/zdesktop-p068.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p068}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]desktop( |$)|[w]lshm|[w]ltest|[m]view|[z]desktop-terminal" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]desktop( |$)|[w]lshm|[w]ltest|[m]view|[z]desktop-terminal" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
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

guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/zdesktop/wallpaper.ppm ] && picture=--wallpaper=/usr/share/zdesktop/wallpaper.ppm
/bin/zdesktop --timeout=600 --width=1280 --height=800 --glass $picture --log-frames > /tmp/zdesktop.log 2>&1 </dev/null & sleep 4
/bin/zdesktop-terminal --token=t1 --timeout-s=500 > /tmp/t.log 2>&1 </dev/null & sleep 6; echo started' >/dev/null
guest 'cat /tmp/t.log' | tee "$out/start.txt"
set -- $(guest "grep 'ZWL MAP client=1 ' /tmp/zdesktop.log" | sed -n 's/.* surface=\([0-9]*\) x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2 \3/p')
surface=${1:-0}; tx=${2:-0}; ty=${3:-0}
echo "terminal: surface $surface at $tx,$ty"
expect_log /tmp/t.log 'ZTERM START run=t1'

# 1. The prompt.
pointer move $((tx + 200)) $((ty + 100)) sleep 300 down sleep 60 up sleep 300
pointer move 1250 780 sleep 500
check "$out/prompt.png" --expect $((tx + 400)),$((ty + 300)),1d2230 || status=1

# 2. A command's output.
keys 'echo ZTERM-OK; ls /\n'
sleep 3
check "$out/output.png" >/dev/null

# 3. Docked: the terminal takes the full size.
pointer move $((tx + 150)) $((ty - 8 - 22)) sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 100
sleep 3
pointer move 1250 780 sleep 500
keys 'ls -l /bin | head -40\n'
sleep 3
check "$out/docked.png" >/dev/null
expect_log /tmp/t.log 'ZTERM RESIZE run=t1'

# 4. exit ends the shell and the terminal.
keys 'exit\n'
sleep 3
expect_log /tmp/t.log 'ZTERM DONE run=t1 reason=shell-exited'

guest 'grep -E "FAILED|ERROR" /tmp/t.log /tmp/zdesktop.log' | tee "$out/errors.txt"
[ -s "$out/errors.txt" ] && status=1
guest 'cat /tmp/t.log' > "$out/t.log"
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "p068: PASS" || echo "p068: FAIL"
exit $status
