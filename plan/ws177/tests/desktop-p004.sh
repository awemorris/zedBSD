#!/bin/sh
# ws177-p004 (the desktop's small items) on the Venus guest of plan/ws177/tests/config-amd64-p004.mk (the Settings
# image with the input method), zdesktop --glass at 1280x800, started with
#   plan/ws089/tests/settings-guest.sh start build/<W>/hdd-image.img
#  1. The accent's swatches by the keyboard (Appearance): Tab gives the keyboard to the chosen swatch (ACCENT focus=0),
#     Right twice moves it (ACCENT focus=2), Enter chooses it (ACCENT index=2), Esc takes the keyboard back
#     (ACCENT focus=-1); accent-focus.png shows the keyboard's ring.  Left at the first swatch stays (focus=0).
#  2. The administration's fields are limited (Users, Add): 40 letters typed into the name field leave 32
#     (users-admin-name.png; the field shows no more than 32, read on the picture).
#  3. No ERROR line in zdesktop's log; Settings runs on.
# The pictures are for review (Q1/user): accent-focus.png (the ring), users-admin-name.png (the name cut at 32).
# The IME candidates' accent, the network row lit in the dark appearance and the File Manager Help card's wrapped
# lines are checked on pictures in the UAT (they need the input method's candidates, the network popup and the
# menu bar's Help, which this script does not drive).
# PASS: every "ok" line and the last line desktop-p004: PASS.
#   plan/ws177/tests/desktop-p004.sh [OUTDIR]   (default build/ws177-p004)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws089-run}"
export GUEST_RUNTIME
out=${1:-build/ws177-p004}
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

# Starts settings on a page (its log in /tmp/s.log) and finds its window.
start_settings() {
	guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/settings --timeout-s=800 $1 > /tmp/s.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
	find_window
	echo "settings: window at $wx,$wy"
}

# A picture of the screen with the pointer out of the way.
shot() {
	pointer move 1270 790 sleep 600
	check "$out/$1" >/dev/null
	echo "shot: $out/$1"
}

wait_guest
guest "$stop_all" >/dev/null
guest "rm -f $conf" >/dev/null
guest "$start_desktop" >/dev/null
wait_desktop

# 1. The accent's swatches by the keyboard.
start_settings appearance
pointer move $((wx + 600)) $((wy + 300)) click left sleep 500
keys '<tab>'
expect_count 'ACCENT focus=0$' 1 "Tab gives the keyboard to the chosen swatch"
keys '<left>'
expect_count 'ACCENT focus=0$' 2 "Left at the first swatch stays"
keys '<right>' '<right>'
expect_count 'ACCENT focus=2$' 1 "Right twice moves it to the third"
shot accent-focus.png
keys '<ret>'
expect_count 'ACCENT index=2$' 1 "Enter chooses the swatch that has the keyboard"
keys '<esc>'
expect_count 'ACCENT focus=-1$' 1 "Esc takes the keyboard back"
guest "$stop_all" >/dev/null
guest "$start_desktop" >/dev/null
wait_desktop

# 2. The administration's name field is limited to 32 bytes.
start_settings users
admin=$(guest "grep -E 'LAYOUT' /tmp/s.log | tail -1")
printf '%s\n' "$admin" > "$out/users-layout.txt"
echo "note: open Users > Add by the layout lines in $out/users-layout.txt if the click below misses"
pointer move $((wx + 700)) $((wy + 160)) click left sleep 800
keys abcdefghijklmnopqrstuvwxyzabcdefghijklmn
shot users-admin-name.png

# 3. Nothing failed.
guest 'cat /tmp/zdesktop.log' > "$out/zdesktop.log"
guest 'cat /tmp/s.log' > "$out/settings.log"
if grep -qE 'KWL FAILED|ERROR' "$out/zdesktop.log"; then echo "FAIL: the compositor logged a failure"; status=1; else echo "ok: no failure in zdesktop's log"; fi
alive=$(guest 'ps -A -o args | grep -cE "^/bin/settings( |$)"' | tail -1)
if [ "${alive:-0}" -gt 0 ] 2>/dev/null; then echo "ok: Settings runs on"; else echo "FAIL: Settings is gone"; status=1; fi
guest "$stop_all" >/dev/null

[ $status = 0 ] && echo "desktop-p004: PASS" || echo "desktop-p004: FAIL"
exit $status
