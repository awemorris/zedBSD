#!/bin/sh
# ws177-p007 (the Welcome's semi-normal paths) on the Venus guest of plan/ws177/tests/config-amd64-p004.mk (the
# Settings image with Files), zdesktop --glass at 1280x800, started with
#   plan/ws089/tests/settings-guest.sh start build/<W>/hdd-image.img
#  1. settings --welcome; Enter five times goes to the Done step (WELCOME step=5 name=done).
#  2. /bin/files moved aside (in the guest), Enter on Done: welcome.done set (WELCOME done error=0), Files cannot be
#     started (WELCOME files-failed error=N), Settings stays and says so (welcome-files-failed.png, review).
#  3. Enter again (the button is Close now): Settings ends without starting Files (no second WELCOME files line).
#  4. /bin/files put back; no failure in zdesktop's log.
# The keys, the narrow window, the Network step's notes and Esc are the host test's (plan/ws164/tests/run-host-welcome.sh).
# PASS: every "ok" line and the last line welcome-p007: PASS.
#   plan/ws177/tests/welcome-p007.sh [OUTDIR]   (default build/ws177-p007)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws089-run}"
export GUEST_RUNTIME
out=${1:-build/ws177-p007}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@" >/dev/null; sleep 0.7; }
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[s]ettings" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland( |$)|[s]ettings" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
start_desktop='export XDG_RUNTIME_DIR=/tmp HOME=/root; rm -f /tmp/wayland-0
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass --wallpaper=/usr/share/keiland/wallpaper.png > /tmp/zdesktop.log 2>&1 </dev/null & sleep 4; echo started'
conf=/root/.config/keiland/desktop.conf
status=0
. plan/ws089/tests/settings-wait.sh

# Counts Settings' lines matching a pattern.
count_log() {
	guest "grep -cE '$1' /tmp/s.log" | tail -1
}

# Waits until Settings' log has COUNT lines matching a pattern (up to 10 s); fails the run otherwise.
expect_count() {
	tries=0
	found=0
	while [ $tries -lt 10 ]; do
		found=$(count_log "$1")
		[ "${found:-0}" -ge "$2" ] 2>/dev/null && { echo "ok: $3"; return 0; }
		tries=$((tries + 1))
		sleep 1
	done
	echo "FAIL: $3 (found ${found:-0} of $2)"
	status=1
	return 1
}

# Tells whether Settings runs (1) or not (0).
settings_alive() {
	guest 'ps -A -o args | grep -cE "^/bin/settings( |$)"' | tail -1
}

wait_guest
guest "$stop_all" >/dev/null
guest "rm -f $conf; test -f /bin/files.p007 && mv /bin/files.p007 /bin/files; echo ready" >/dev/null
guest "$start_desktop" >/dev/null
wait_desktop

# 1. The Welcome, to its last step by Enter.
guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/settings --timeout-s=600 --welcome > /tmp/s.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
find_window
pointer move $((wx + 400)) $((wy + 20)) sleep 200 down sleep 60 up sleep 500
expect_count 'WELCOME step=0 name=welcome' 1 "the Welcome starts"
keys '<ret>' '<ret>' '<ret>' '<ret>' '<ret>'
expect_count 'WELCOME step=5 name=done' 1 "Enter goes to the Done step"

# 2. Files cannot be started: Settings stays and says so.
guest 'mv /bin/files /bin/files.p007; echo moved' >/dev/null
keys '<ret>'
expect_count 'WELCOME done error=0' 1 "Start using Kei sets welcome.done"
expect_count 'WELCOME files-failed error=[0-9]+' 1 "Files could not be started"
sleep 1
alive=$(settings_alive)
[ "${alive:-0}" -gt 0 ] 2>/dev/null && echo "ok: Settings stays to say so" || { echo "FAIL: Settings closed"; status=1; }
pointer move 1270 790 sleep 600
check "$out/welcome-files-failed.png" >/dev/null
echo "shot: $out/welcome-files-failed.png"

# 3. Close: Settings ends, Files not started again.
keys '<ret>'
sleep 2
alive=$(settings_alive)
[ "${alive:-0}" = 0 ] && echo "ok: Close ends Settings" || { echo "FAIL: Settings still runs"; status=1; }
lines=$(count_log 'WELCOME files error=')
[ "${lines:-0}" = 1 ] && echo "ok: Files was not started again" || { echo "FAIL: Files started ${lines:-0} times"; status=1; }

# 4. Files back; nothing failed.
guest 'mv /bin/files.p007 /bin/files; echo back' >/dev/null
guest 'cat /tmp/zdesktop.log' > "$out/zdesktop.log"
guest 'cat /tmp/s.log' > "$out/settings.log"
if grep -qE 'KWL FAILED' "$out/zdesktop.log"; then echo "FAIL: the compositor failed"; status=1; else echo "ok: no KWL FAILED"; fi
guest "$stop_all" >/dev/null

[ $status = 0 ] && echo "welcome-p007: PASS" || echo "welcome-p007: FAIL"
exit $status
