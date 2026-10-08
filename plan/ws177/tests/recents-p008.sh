#!/bin/sh
# ws177-p008 (another program's change of the recent list reaches Text Editor's Open Recent) on the Venus guest of
# plan/ws177/tests/config-amd64-p008.mk, zdesktop --glass at 1280x800, started with
#   plan/ws089/tests/settings-guest.sh start build/<W>/hdd-image.img
#  1. A recent list of two of Text Editor's files; Text Editor starts and reads it (TEXTEDIT READY).
#  2. The list is emptied as Files' Clear Recents does (a new file renamed over it, in the guest's shell).
#  3. Settings maps a window on top (the keyboard goes to it) and quits (the keyboard comes back to Text Editor's
#     window, the top one left): the window gets the keyboard and reads the list again (RECENT changed: read again);
#     a second focus with nothing changed reads nothing (still one such line).  T1-411 clicked the desktop and then
#     the window, but this --testing compositor runs no desktop program, so a click on the background never took the
#     keyboard (no KWL DESKTOP focus) and the window never got it again.
#  4. No KWL FAILED; Text Editor runs on.
# Files' question before Clear Recents and its view of a stopped list are the host test's
# (plan/tools/files/run-host-files-recents.sh); the stamp itself is plan/ws177/tests/host-recent-stamp.sh's.
# PASS: every "ok" line and the last line recents-p008: PASS.
#   plan/ws177/tests/recents-p008.sh [OUTDIR]   (default build/ws177-p008)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws089-run}"
export GUEST_RUNTIME
out=${1:-build/ws177-p008}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[t]extedit|[s]ettings" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland( |$)|[t]extedit|[s]ettings" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
start_desktop='export XDG_RUNTIME_DIR=/tmp HOME=/root; rm -f /tmp/wayland-0
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass --wallpaper=/usr/share/keiland/wallpaper.png > /tmp/zdesktop.log 2>&1 </dev/null & sleep 4; echo started'
list=/root/.local/share/keiland/recent
status=0
. plan/ws089/tests/settings-wait.sh

# Counts Text Editor's lines matching a pattern.
count_log() {
	guest "grep -cE '$1' /tmp/te.log" | tail -1
}

# Waits until Text Editor's log has COUNT lines matching a pattern (up to 10 s); fails the run otherwise.
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

# Takes the keyboard from Text Editor and gives it back: Settings maps a window on top, then is ended.
focus_away_and_back() {
	maps=$(guest "grep -c 'KWL MAP client=' /tmp/zdesktop.log" | tail -1)
	guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/settings --timeout-s=60 about > /tmp/s.log 2>&1 </dev/null & echo started" >/dev/null
	tries=0
	while [ $tries -lt 15 ]; do
		now=$(guest "grep -c 'KWL MAP client=' /tmp/zdesktop.log" | tail -1)
		[ "${now:-0}" -gt "${maps:-0}" ] 2>/dev/null && break
		tries=$((tries + 1))
		sleep 1
	done
	[ $tries -lt 15 ] && echo "settings: mapped on top" || { echo "FAIL: Settings mapped no window"; status=1; }
	sleep 1
	guest 'for p in $(ps -A -o pid,args | grep -E "[s]ettings" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[s]ettings" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done; echo gone' >/dev/null
	sleep 1
}

wait_guest
guest "$stop_all" >/dev/null
guest "$start_desktop" >/dev/null
wait_desktop

# 1. Two of Text Editor's files in the list, and Text Editor.
guest "mkdir -p /root/.local/share/keiland; echo one > /tmp/p008-a.txt; echo two > /tmp/p008-b.txt; rm -f $list.off
printf '1700000000\ttextedit\t/tmp/p008-a.txt\n1700000001\ttextedit\t/tmp/p008-b.txt\n' > $list; echo ready" >/dev/null
guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=300 > /tmp/te.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
expect_count 'TEXTEDIT READY' 1 "Text Editor starts"
find_window
echo "textedit: window at $wx,$wy"

# 2. The list emptied as Files does it (a new file renamed over the old one).
guest ": > $list.new; mv $list.new $list; echo emptied" >/dev/null

# 3. The keyboard away to another window and back: the list read again, once.
focus_away_and_back
expect_count 'RECENT changed: read again' 1 "the window's focus reads the changed list again"
focus_away_and_back
sleep 2
lines=$(count_log 'RECENT changed: read again')
[ "${lines:-0}" = 1 ] && echo "ok: an unchanged list is not read again" || { echo "FAIL: read again ${lines:-0} times"; status=1; }

# 4. Nothing failed.
guest 'cat /tmp/zdesktop.log' > "$out/zdesktop.log"
guest 'cat /tmp/te.log' > "$out/textedit.log"
if grep -q 'KWL FAILED' "$out/zdesktop.log"; then echo "FAIL: the compositor failed"; status=1; else echo "ok: no KWL FAILED"; fi
alive=$(guest 'ps -A -o args | grep -cE "^/bin/textedit( |$)"' | tail -1)
if [ "${alive:-0}" -gt 0 ] 2>/dev/null; then echo "ok: Text Editor runs on"; else echo "FAIL: Text Editor is gone"; status=1; fi
guest "$stop_all" >/dev/null

[ $status = 0 ] && echo "recents-p008: PASS" || echo "recents-p008: FAIL"
exit $status
