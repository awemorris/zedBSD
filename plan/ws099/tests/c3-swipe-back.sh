#!/bin/sh
# ws099-p015 (C3): a fullscreen window goes back to a window with a swipe up from the bottom edge, on the Venus guest.
# zdesktop --glass at 1280x800, the pointer driven through QMP, the fingers (step 7) with /bin/touchinject.
#  1. The top-right swipe brings Notes fullscreen (KWL CORNER commit).  fullscreen.png.
#  2. A swipe up from the bottom edge (200 pixels in 10 steps) takes it back to a window (KWL GLASS unfullscreen
#     ... via=swipe, KWL WINDOW unfullscreen ... placed=0), not Wiseview (no KWL WISEVIEW open): it had no place as
#     a window, so it is centred with its title bar under the system bar (y >= 98, BUG-114's rule), and Notes hears
#     it (its next configure has fullscreen=0).  window.png.
#  3. Its title bar drags it (KWL GLASS moved).  moved.png.
#  4. The top-right swipe again (the compositor makes it fullscreen), and the bottom swipe again: back where it was
#     moved (placed=1).  back.png.
#  5. A stroke that starts above the bottom edge is Notes' (no unfullscreen).
#  6. The bottom-right corner's swipe over fullscreen Notes is still the on-screen keyboard's (WS102, KWL OSK open
#     kind=flick), not the bottom edge's; the same swipe closes it.  osk.png.
#  7. With /bin/touchinject in the image (the WS079 demo image): a finger's swipe up from the bottom edge takes
#     the fullscreen window back too (KWL GLASS unfullscreen-swipe start ... source=2).  touch.png.
# Prints "c3-swipe-back: PASS" or "c3-swipe-back: FAIL".
#
#   plan/ws035/tests/zdesktop-guest.sh start IMAGE     (the guest must be up)
#   plan/ws099/tests/c3-swipe-back.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws035-sq-run}"
export GUEST_RUNTIME
out=${1:-build/ws099-shots/c3-swipe-back}
mkdir -p "$out"
# The SSH to the guest, tried again when ssh itself fails (plan/ws099/tests/guest-retry.sh, ws099-p023).
. plan/ws099/tests/guest-retry.sh
guest() { guest_retry 90 "$1" </dev/null; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|/bin/[n]otes" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland( |$)|/bin/[n]otes" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
status=0
top=98

# The number of lines of zdesktop's log matching a pattern.  An answer that is not a number (the SSH timed out with
# nothing, T1-477's step 6: "found 2 (more than 0)" with both earlier swipes in the log) is asked again, three times
# in all, and reported on stderr as the harness's.
count() {
	count_try=1
	while :; do
		count_reply=$(guest "grep -cE '$1' /tmp/zdesktop.log" | tail -1)
		case $count_reply in
		''|*[!0-9]*) ;;
		*) echo "$count_reply"; return 0 ;;
		esac
		echo "harness: count '$1' got '$count_reply', attempt $count_try" >&2
		[ $count_try -ge 3 ] && break
		count_try=$((count_try + 1))
		sleep 2
	done
	echo "$count_reply"
}

# Fails the run unless zdesktop's log has (within a few seconds) as many lines matching a pattern as asked (default 1).
expect_log() {
	tries=0
	found=0
	while [ $tries -lt 8 ]; do
		found=$(count "$1")
		[ "${found:-0}" -ge "${2:-1}" ] 2>/dev/null && break
		tries=$((tries + 1))
		sleep 1
	done
	if [ "${found:-0}" -ge "${2:-1}" ] 2>/dev/null; then
		echo "log: $1 (${2:-1}) ok"
	else
		echo "log: $1 (${2:-1}) MISSING (found ${found:-0})"
		status=1
	fi
}

# Fails the run when zdesktop's log has more than a number of lines matching a pattern.
expect_no_more() {
	found=$(count "$1")
	if [ "${found:-0}" -le "$2" ] 2>/dev/null; then
		echo "log: no more $1 ok"
	else
		echo "log: $1 found $found (more than $2) FAIL"
		status=1
	fi
}

# A drag from one point to another in n steps of ms milliseconds.
stroke() {
	x0=$1; y0=$2; x1=$3; y1=$4; n=$5; ms=$6
	steps="move $x0 $y0 sleep 200 down sleep 30"
	i=1
	while [ $i -le $n ]; do
		steps="$steps move $((x0 + (x1 - x0) * i / n)) $((y0 + (y1 - y0) * i / n)) sleep $ms"
		i=$((i + 1))
	done
	echo "$steps"
}

guest "$stop_all" >/dev/null
guest 'rm -rf /root/Documents/Notes /root/.local/share/keiland/notes; export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=600 --width=1280 --height=800 --glass $picture > /tmp/zdesktop.log 2>&1 </dev/null & for w in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do grep -q KWL.READY /tmp/zdesktop.log 2>/dev/null && break; sleep 0.5; done; sleep 1; echo started' >/dev/null

# 1. The top-right swipe: Notes fullscreen.
pointer $(stroke 1272 6 1072 206 10 30) up sleep 5000
expect_log 'KWL CORNER commit'
expect_log 'KWL CONFIGURE client=[0-9]+ surface=[0-9]+ serial=[0-9]+ width=1280 height=800 fullscreen=1'
pointer move 640 400 sleep 300
check "$out/fullscreen.png" >/dev/null

# 2. The bottom edge's swipe up: a window again, centred under the system bar, and not Wiseview.
wiseviews=$(count 'KWL WISEVIEW open')
pointer $(stroke 640 797 640 597 10 30) up sleep 2000
expect_log 'KWL GLASS unfullscreen-swipe start'
expect_log 'KWL GLASS unfullscreen surface=[0-9]+ via=swipe errno=0'
expect_log 'KWL WINDOW unfullscreen surface=[0-9]+ x=-?[0-9]+ y=-?[0-9]+ placed=0'
expect_log 'KWL CONFIGURE client=[0-9]+ surface=[0-9]+ serial=[0-9]+ width=[0-9]+ height=[0-9]+ fullscreen=0'
expect_log 'KWL WINDOW centred surface=[0-9]+ x=[0-9]+ y=[0-9]+ width=[0-9]+ height=[0-9]+'
expect_no_more 'KWL WISEVIEW open' "${wiseviews:-0}"
set -- $(guest "grep 'KWL WINDOW centred' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
nx=${1:-0}; ny=${2:-0}; nw=${3:-0}; nh=${4:-0}
echo "notes centred at $nx,$ny size ${nw}x$nh"
if [ "$ny" -ge "$top" ] 2>/dev/null; then
	echo "title bar under the system bar (y=$ny >= $top) ok"
else
	echo "title bar under the system bar (y=$ny >= $top) FAIL"
	status=1
fi
pointer move 1270 790 sleep 600
check "$out/window.png" >/dev/null

# 3. Its title bar drags it.
tx=$((nx + nw - 250)); ty=$((ny - 30))
pointer move $((tx - 2)) $ty sleep 200 move $tx $ty sleep 300 down sleep 200 move $((tx + 40)) $((ty + 20)) sleep 150 move $((tx + 100)) $((ty + 60)) sleep 300 up sleep 800
expect_log "KWL GLASS moved surface=[0-9]+ x=$((nx + 100)) y=$((ny + 60))"
pointer move 1270 790 sleep 600
check "$out/moved.png" >/dev/null

# 4. Fullscreen again from the corner, and back again with the bottom swipe: where it was moved, kept inside the space.
pointer $(stroke 1272 6 1072 206 10 30) up sleep 3000
expect_log 'KWL CORNER commit' 2
pointer $(stroke 640 797 640 597 10 30) up sleep 2000
expect_log 'KWL GLASS unfullscreen surface=[0-9]+ via=swipe errno=0' 2
space_bottom=$((800 - 12))
want_y=$((ny + 60))
[ $((want_y + nh)) -gt $space_bottom ] && want_y=$((space_bottom - nh))
[ $want_y -lt $top ] && want_y=$top
expect_log "KWL WINDOW unfullscreen surface=[0-9]+ x=$((nx + 100)) y=$want_y placed=1"
pointer move 1270 790 sleep 600
check "$out/back.png" >/dev/null

# 5. Fullscreen again; a stroke that starts above the bottom edge is Notes' own.
pointer $(stroke 1272 6 1072 206 10 30) up sleep 3000
expect_log 'KWL CORNER commit' 3
swipes=$(count 'KWL GLASS unfullscreen surface=')
pointer $(stroke 640 760 640 560 10 30) up sleep 1500
expect_no_more 'KWL GLASS unfullscreen surface=' "${swipes:-0}"
check "$out/stroke.png" >/dev/null

# 6. The bottom-right corner is the keyboard's over fullscreen Notes (WS102): the flick panel opens, and closes.
swipes=$(count 'KWL GLASS unfullscreen-swipe start')
opens=$(count 'KWL OSK open kind=flick')
pointer move 1272 792 sleep 200 down sleep 80 move 1201 721 sleep 60 move 1130 650 sleep 120 up sleep 900
expect_log 'KWL OSK open kind=flick' $((opens + 1))
expect_no_more 'KWL GLASS unfullscreen-swipe start' "${swipes:-0}"
pointer move 700 300 sleep 400
check "$out/osk.png" >/dev/null
pointer move 1272 792 sleep 200 down sleep 80 move 1201 721 sleep 60 move 1130 650 sleep 120 up sleep 900
expect_log 'KWL OSK close kind=flick'

# 7. A finger's swipe up from the bottom edge, when the image has the touch injector.
if [ "$(guest 'test -x /bin/touchinject && echo yes' | tail -1)" = yes ]; then
	backs=$(count 'KWL GLASS unfullscreen surface=')
	printf 'size 1279 799 2\nwait 3000\ndown 1 640 797\nswipe 0 -200 10 30\nup 1\nwait 800\n' > "$out/touch.script"
	tries=0
	until timeout 60 python3 plan/tools/guest/guest.py put "$out/touch.script" /tmp/c3-touch.script >/dev/null 2>&1 </dev/null &&
	    [ "$(guest 'test -s /tmp/c3-touch.script && echo put' | tail -1)" = put ] || [ $tries -ge 3 ]; do
		tries=$((tries + 1))
		sleep 2
	done
	guest 'timeout 60 /bin/touchinject /tmp/c3-touch.script; echo replay=$?' | grep -q '^replay=0$' || { echo "touchinject: FAILED"; status=1; }
	expect_log 'KWL GLASS unfullscreen-swipe start y=79[0-9] source=2'
	expect_log 'KWL GLASS unfullscreen surface=[0-9]+ via=swipe errno=0' $((backs + 1))
	pointer move 1270 790 sleep 600
	check "$out/touch.png" >/dev/null
else
	echo "touch: SKIPPED (no /bin/touchinject in the image)"
fi

# Nothing failed.
guest 'grep -E "ERROR|FAILED|protocol error" /tmp/zdesktop.log' | tee "$out/errors.txt"
[ -s "$out/errors.txt" ] && status=1
guest 'grep -E "KWL (CORNER|WINDOW|MAP|GLASS|CONFIGURE|WISEVIEW|OSK|TOUCH)|NOTES (START|LAYOUT)" /tmp/zdesktop.log' > "$out/zdesktop.log"
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "c3-swipe-back: PASS" || echo "c3-swipe-back: FAIL"
exit $status
