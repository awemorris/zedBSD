#!/bin/sh
# ws177-p001 (wl_surface.enter and leave: the client hears which display its window is on) on the Venus guest with two
# heads through QEMU's D-Bus display.  The image is plan/ws177/tests/config-amd64-p001.mk's (Files, keiland-system),
# started with
#   VENUS_DISPLAY=dbus VENUS_OUTPUTS=2 plan/tools/files/files-guest.sh start IMAGE
# Head 0 is 1280x800 (the anchor, output 0), head 1 1024x768 right of it (output 1).  The judgement is by the
# compositor's lines: KWL SURFACE enter|leave surface=S output=N client=C is one change of the set of outputs the
# client was told its surface is on (each change once, sent to every wl_output binding of that display the client
# holds); KWL WINDOW output ... why=... is the window given to an output (ws113-p007).
#  1. A Files window opens on the anchor: it enters output 0 and nothing else.
#  2. Super+Shift+Right gives it to head 1: it enters output 1, and after that leaves output 0.  Super+Shift+Left: it
#     enters output 0, then leaves output 1.
#  3. On head 1, the head unplugged: it leaves output 1 and enters output 0 (the retreat).
#  4. Plugged again, the mirror chosen (keiland-system display-mode mirror): the window, on the anchor, enters output 1
#     too and does not leave output 0; the extended mode again: it leaves output 1.
#  5. No KWL FAILED; the compositor and Files run on (Files decoded every enter and leave it was sent).
# PASS: every "ok" line and the last line displays-p001: PASS.
#   plan/ws177/tests/displays-p001.sh [OUTDIR]     (default build/ws177-p001-guest)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws071-run}"
export GUEST_RUNTIME
out=${1:-build/ws177-p001-guest}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null | tr -d '\r'; }
head_set() { sh plan/tools/guest/venus-head.sh "$1" "$2"; }
probe() { guest "XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-0 HOME=/tmp/p001-home /bin/keiland-system --timeout-ms=5000 $1"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; sleep 1; }
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[d]isplay-events|[f]iles" | awk "{print \$1}"); do kill $p; done; sleep 1'
start_compositor='export XDG_RUNTIME_DIR=/tmp HOME=/tmp/p001-home; rm -f /tmp/wayland-0
picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
nohup /bin/wayland --testing --timeout=600 --glass $picture > /tmp/zdesktop.log 2>&1 </dev/null & echo started'
start_files='export XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-0 HOME=/tmp/p001-home
nohup /bin/files --timeout-s=500 --width=800 --height=500 > /tmp/f.log 2>&1 </dev/null & echo started'
status=0
. plan/ws089/tests/settings-wait.sh

# Fails the run unless a text has a line matching a pattern.
expect_text() {
	if printf '%s\n' "$1" | grep -qE "$2"; then echo "ok: $3"; else echo "FAIL: $3"; status=1; fi
}

# Waits (up to SECONDS) until a guest file has a line matching a pattern, keeping a copy in OUTDIR.
wait_line() {
	waited=0
	while [ "$waited" -lt "$3" ]; do
		guest "cat $1" > "$out/$(basename "$1")"
		if grep -qE "$2" "$out/$(basename "$1")"; then return 0; fi
		sleep 1
		waited=$((waited + 1))
	done
	echo "note: no line /$2/ in $1 within $3 s"
	return 1
}

# Counts the compositor's lines matching a pattern.
count_lines() {
	guest "grep -cE '$1' /tmp/zdesktop.log" | tail -1
}

# Gives the number of the compositor's last line matching a pattern (0 for none).
last_line() {
	found=$(guest "grep -nE '$1' /tmp/zdesktop.log | tail -1 | cut -d: -f1" | tail -1)
	echo "${found:-0}"
}

# Waits until the compositor has COUNT lines matching a pattern (up to 10 s); fails the run otherwise.
expect_count() {
	waited=0
	found=0
	while [ "$waited" -lt 10 ]; do
		found=$(count_lines "$1")
		[ "${found:-0}" -ge "$2" ] 2>/dev/null && { echo "ok: $3"; return 0; }
		sleep 1
		waited=$((waited + 1))
	done
	echo "FAIL: $3 (found ${found:-0} of $2)"
	status=1
	return 1
}

# Fails the run unless the compositor's line FIRST comes before its line SECOND (each its last match).
expect_before() {
	first=$(last_line "$1")
	second=$(last_line "$2")
	if [ "$first" -gt 0 ] 2>/dev/null && [ "$second" -gt "$first" ] 2>/dev/null; then
		echo "ok: $3 (line $first before line $second)"
	else
		echo "FAIL: $3 (line $first, line $second)"
		status=1
	fi
}

# The patterns of the window's changes.
enter_on() { echo "KWL SURFACE enter surface=$surface output=$1 client=$client\$"; }
leave_from() { echo "KWL SURFACE leave surface=$surface output=$1 client=$client\$"; }

# The guest, the heads and the compositor.
wait_guest
guest "$stop_all" >/dev/null
guest 'rm -rf /tmp/p001-home; mkdir -p /tmp/p001-home' >/dev/null
head_set 0 1280x800
head_set 1 1024x768
sleep 3
guest "$start_compositor" >/dev/null
wait_line /tmp/zdesktop.log 'KWL OUTPUT head open name=Venus virtual display 1 ' 60
wait_desktop

# 1. A Files window on the anchor.
guest "$start_files" >/dev/null
find_window
surface=$wsurface
client=$wclient
echo "files: surface ${surface:-none} client ${client:-none}"
[ -n "$surface" ] || { echo "FAIL: no Files window"; status=1; surface=0; client=0; }
expect_count "$(enter_on 0)" 1 "the new window enters output 0"
others=$(count_lines "KWL SURFACE (enter|leave) surface=$surface output=1 client=$client\$")
[ "${others:-0}" = 0 ] && echo "ok: the new window is told of no other output" || { echo "FAIL: the new window was told of output 1"; status=1; }

# 2. To head 1 and back by the keyboard: the new output is entered before the old one is left.
keys '<super-shift-right>'
expect_count "$(enter_on 1)" 1 "Super+Shift+Right: the window enters output 1"
expect_count "$(leave_from 0)" 1 "and leaves output 0"
expect_before "$(enter_on 1)" "$(leave_from 0)" "output 1 is entered before output 0 is left"
keys '<super-shift-left>'
expect_count "$(enter_on 0)" 2 "Super+Shift+Left: the window enters output 0 again"
expect_count "$(leave_from 1)" 1 "and leaves output 1"
expect_before "$(enter_on 0)" "$(leave_from 1)" "output 0 is entered before output 1 is left"

# 3. On head 1, the head unplugged: back on the anchor.
keys '<super-shift-right>'
expect_count "$(enter_on 1)" 2 "the window is on output 1 before the unplugging"
head_set 1 off
expect_count "KWL WINDOW output surface=$surface .*output=0 .*why=retreat" 1 "the unplugging brings the window to the anchor"
expect_count "$(leave_from 1)" 2 "the window leaves the unplugged output 1"
expect_count "$(enter_on 0)" 3 "and enters output 0"

# 4. Plugged again; the mirror puts the window on both displays, the extended mode on the anchor alone.
head_set 1 1024x768
wait_line /tmp/zdesktop.log 'KWL OUTPUT head open name=Venus virtual display 1 .*' 30
sleep 2
leaves=$(count_lines "$(leave_from 0)")
text=$(probe 'display-mode mirror')
printf '%s\n' "$text" > "$out/probe-mirror.txt"
expect_text "$text" 'KEILAND-SYSTEM result request=[0-9]+ error=0' "the mirror is applied"
expect_count "$(enter_on 1)" 3 "in the mirror the window enters output 1 too"
after=$(count_lines "$(leave_from 0)")
[ "${after:-0}" = "${leaves:-0}" ] && echo "ok: in the mirror the window stays on output 0" || { echo "FAIL: the mirror made the window leave output 0"; status=1; }
text=$(probe 'display-mode extended')
printf '%s\n' "$text" > "$out/probe-extended.txt"
expect_count "$(leave_from 1)" 3 "the extended mode: the window leaves output 1"

# 5. Nothing failed.
guest 'cat /tmp/zdesktop.log' > "$out/zdesktop.log"
guest 'cat /tmp/f.log' > "$out/files.log"
if grep -q 'KWL FAILED' "$out/zdesktop.log"; then echo "FAIL: the compositor failed"; status=1; else echo "ok: no KWL FAILED"; fi
alive=$(guest 'ps -A -o args | grep -cE "^/bin/wayland( |$)"' | tail -1)
if [ "${alive:-0}" -gt 0 ] 2>/dev/null; then echo "ok: the compositor runs on"; else echo "FAIL: the compositor is gone"; status=1; fi
alive=$(guest 'ps -A -o args | grep -cE "^/bin/files( |$)"' | tail -1)
if [ "${alive:-0}" -gt 0 ] 2>/dev/null; then echo "ok: Files runs on"; else echo "FAIL: Files is gone"; status=1; fi
guest "$stop_all" >/dev/null

[ $status = 0 ] && echo "displays-p001: PASS" || echo "displays-p001: FAIL"
exit $status
