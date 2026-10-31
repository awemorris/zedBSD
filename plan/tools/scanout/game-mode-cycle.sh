#!/bin/sh
# The compositor's game mode and window mode, back and forth, on the Venus guest (QEMU, whose display shows a
# client's GPU buffer straight, so the game mode is entered as on the VisionFive 2 and the i915).
#
#   plan/tools/scanout/game-mode-cycle.sh IMAGE OUTDIR [ROUNDS]     (ROUNDS default 6)
#
# IMAGE: a Venus guest image with the compositor and mview (plan/tools/titlebar/build-menu-image.sh BUILD).  The
# host's Venus renderer as plan/ws035/tests/zdesktop-guest.sh says (build/ws035-sq-venus, the vgem render node).
# The compositor runs as root (--testing, /tmp/kwl.log in the guest).  The pointer is moved once, so the arrow shows
# in window mode.  Then mview --content=game --spin=6 runs ROUNDS times: each run's fullscreen window enters the
# game mode once the pointer has been still 2 s (KWL SCANOUT direct=1) and leaves it when mview exits
# (direct=0 reason=no-window), and window mode opens again (KWL MODE window); the pointer moved over the game in the
# first round does not leave the game mode (a game shows no cursor).  In the first round the screen is photographed
# in window mode (window.png, the arrow), in the game mode (game-direct.png) and after the pointer moved
# (game-pointer.png).  The Venus head's VNC does not show a direct scanout (it keeps the last composed picture), so
# those two show only that nothing was composed meanwhile; the cursor over a composed fullscreen game is seen with
# the compositor's --rotate=180 (the game mode is not entered on a turned output).
#
# Judged by the guest's own files over SSH and by the Venus head's pictures (VNC), never by the emulator's console.
# PASS: the compositor runs after every round, each round has direct=1, no-window and MODE window, no
# KWL FAILED / MODE_ERROR, and no direct=0 reason=pointer.  The pictures are looked at by a person or an agent.  GUEST_RUNTIME (default
# build/scanout-run) holds the guest's copy of the disk.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
[ $# -ge 2 ] || { echo "usage: game-mode-cycle.sh IMAGE OUTDIR [ROUNDS]" >&2; exit 2; }
image=$1 out=$2 rounds=${3:-6}
mkdir -p "$out"
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/scanout-run}"
G=plan/tools/guest/guest.py
g() { timeout "${T:-90}" python3 $G run "$1" 2>&1; }
qmp() { timeout 30 python3 plan/tools/files/qmp-input.py --width 1280 --height 800 "$GUEST_RUNTIME/qmp.sock" "$@" >/dev/null; }
shot() { timeout 60 python3 plan/ws035/tests/zdesktop-shot.py "$out/$1.png" --runtime "$GUEST_RUNTIME" >/dev/null 2>&1 || echo "shot $1 failed"; }
count() { g "grep -cE '$1' /tmp/kwl.log" | tail -1; }
timeout 900 plan/ws035/tests/zdesktop-guest.sh start "$image" > "$out/start.txt" 2>&1 || { echo "game-mode-cycle: the guest did not start"; exit 1; }
timeout 300 python3 $G wait --timeout 240 >/dev/null 2>&1 || { echo "game-mode-cycle: no SSH"; timeout 60 python3 $G stop >/dev/null; exit 1; }
ready=$(g 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
/bin/wayland --testing --timeout=3000 --width=1280 --height=800 > /tmp/kwl.log 2>&1 </dev/null & for w in $(seq 1 60); do grep -q "KWL READY" /tmp/kwl.log 2>/dev/null && break; sleep 0.5; done; grep -c "KWL READY" /tmp/kwl.log' | tail -1)
[ "$ready" = 1 ] || { echo "game-mode-cycle: the compositor did not start"; timeout 60 plan/ws035/tests/zdesktop-guest.sh stop >/dev/null 2>&1; exit 1; }
sleep 4
qmp move 600 380 sleep 200 move 640 400
sleep 2
shot window
status=0
round=1
while [ "$round" -le "$rounds" ]; do
	before_direct=$(count 'KWL SCANOUT direct=1')
	before_left=$(count 'KWL SCANOUT direct=0 reason=no-window')
	before_window=$(count 'KWL MODE window')
	g "export XDG_RUNTIME_DIR=/tmp; (/bin/mview --token=r$round --spin=8 --content=game --present=mailbox --images=4 > /tmp/mview-r$round.log 2>&1 &)" >/dev/null
	if [ "$round" = 1 ]; then
		# The first mview loads its model (about 50 s under Venus on a loaded host); up to 120 s.
		T=150 g 'for w in $(seq 1 240); do grep -q "KWL SCANOUT direct=1" /tmp/kwl.log && break; sleep 0.5; done' >/dev/null
		sleep 1
		shot game-direct
		qmp move 700 450 sleep 100 move 720 460 sleep 100 move 740 470
		sleep 1
		shot game-pointer
	fi
	T=150 g "for w in \$(seq 1 240); do grep -q 'MVIEW DONE' /tmp/mview-r$round.log && break; sleep 0.5; done; sleep 3" >/dev/null
	alive=$(g "ps -A -o args | grep -cE '^/bin/wayland '" | tail -1)
	direct=$(( $(count 'KWL SCANOUT direct=1') - before_direct ))
	left=$(( $(count 'KWL SCANOUT direct=0 reason=no-window') - before_left ))
	window=$(( $(count 'KWL MODE window') - before_window ))
	echo "round $round: compositor=$alive direct=$direct no-window=$left window-mode=$window $(g "grep -E 'MVIEW (DONE|FAILED)' /tmp/mview-r$round.log" | tail -1)"
	if [ "$alive" != 1 ] || [ "$direct" -lt 1 ] || [ "$left" -lt 1 ] || [ "$window" -lt 1 ]; then
		status=1
		[ "$alive" = 1 ] || break
	fi
	round=$((round + 1))
done
failed=$(count 'KWL (FAILED|MODE_ERROR)')
[ "$failed" = 0 ] || status=1
pointer=$(count 'KWL SCANOUT direct=0 reason=pointer')
[ "$pointer" = 0 ] || status=1
echo "KWL FAILED/MODE_ERROR lines: $failed; game mode left for the pointer: $pointer"
timeout 60 python3 $G get /tmp/kwl.log "$out/kwl.log" >/dev/null 2>&1
timeout 60 plan/ws035/tests/zdesktop-guest.sh stop >/dev/null 2>&1
echo "pictures: $out/window.png $out/game-direct.png $out/game-pointer.png"
[ $status -eq 0 ] && echo "game-mode-cycle: PASS" || echo "game-mode-cycle: FAIL"
exit $status
