#!/bin/sh
# WS181 (the 2026-10-07 UAT): the windows' states, App Home as a mode of its own and the screen edges' gestures, the
# arrangement menu and mode, on the pen test guest (plan/ws079/tests/config-amd64-pen.mk; plan/ws079/tests/pen-guest.sh
# start IMAGE).  The compositor and wltest under test (BUILD/bin/wayland, BUILD/bin/wltest) are copied in, 1280x800.
# The mouse through QMP, keys through QMP, the touch screen through touchinject.  Three wltest windows a, b, c.
#  A. States (p002, design.md §1.5):
#   1. b docked (double click), Alt+Tab to a (docked too); a brought back by a double click on its title in the bar:
#      every window floats ("KWL LAYOUT leave via=double-click ... quiet=1", "KWL LAYOUT windows ... docked=0").
#   2. A click on b's body changes nothing of its size (no "KWL GLASS undock", no "action=float").
#   3. a docked; a ends (killed): the docked mode ends, the other windows stay windows ("leave via=closed front=0",
#      no "KWL LAYOUT front ... action=dock").
#   4. b docked; c, opened in the docked mode, opens docked and owns the desktop ("KWL LAYOUT owner desktop=2"),
#      and 1.5 s later the mode is still docked (no new "leave").
#   5. c carried to desktop 2 (Ctrl+Alt+Shift+Right): still docked ("carried"), no "leave"; back with
#      Ctrl+Alt+Shift+Left.
#   6. c killed with -9: "leave via=closed".
#  B. Home and the edges (p003, design.md §3.3):
#   7. The mouse's swipe up from the bottom edge opens Home ("KWL HOME open via=edge", home-edge.png shows no desktop;
#      b7-home-half.png, held half way, shows the desktop smaller in the middle and fading over Home's content coming
#      forward, ws181-p008); its bottom edge's swipe does nothing; a drag down closes it ("KWL HOME close via=pull-down").
#      A drag from the top-left corner opens nothing (ws181-p008, WS184 takes it: "KWL HOME press slipped"); a click
#      there opens Home and a second one closes it ("KWL HOME open via=launcher", "close via=launcher").
#   8. A finger's swipe down from the top band opens Wiseview ("KWL WISEVIEW gesture via=top-edge", "opening");
#      a tap closes it.  A finger's tap on the desktops' pill in the band reaches it: the press is given again
#      ("KWL EDGE band replay release=1") and the arrangement menu opens ("KWL ARRANGE menu open"); Esc closes it.
#   9. b docked, its title in the bar pulled sideways 200 px and let go in the bar: it comes off and moves
#      ("leave via=pull"), and is not docked again ("KWL GLASS moved", no new "dock ... via=drag").
#  C. The arrangement (p004, design.md §5.4):
#  10. A click on the desktops' pill opens the menu ("KWL ARRANGE menu open"; it grows from the pill and is grown,
#      "KWL ARRANGE menu settled", ws181-p007; c10-menu-opening.png just after the click, as soon as the picture can
#      be read, c10-arrange-menu.png 0.3 s later: the frosted glass shows the windows under it blurred); "One on the Right"
#      arranges the three windows ("KWL ARRANGE apply layout=right-main desktop=2 windows=3", arranged.png), and each
#      client draws its slot's size ("KWL GLASS resized ... width=W height=H" after the apply, W x H its last configure's).
#  11. The left top slot's window dragged by its title onto the right slot: "KWL ARRANGE swap".
#  12. A double click on a title docks it and ends the mode ("KWL ARRANGE end desktop=2 reason=dock",
#      "KWL LAYOUT mode=docked"); brought back, every window floats ("windows ... docked=0") and a title's drag is a
#      move ("KWL GLASS moved"), not a swap.
#  13. The compositor stays up, with no ERROR in its log.
#
#   plan/ws181/tests/ws181-guest.sh BUILD [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws079-run}"
build=${1:?usage: ws181-guest.sh BUILD [OUTDIR]}
out=${2:-build/ws181-guest}
mkdir -p "$out"
qmp="$GUEST_RUNTIME/qmp.sock"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
put() { timeout 90 python3 plan/tools/guest/guest.py put "$1" "$2" >/dev/null 2>&1; }
send() { timeout 40 python3 plan/ws049/tests/qmp-send.py "$qmp" "$@" >> "$out/qmp.txt" 2>&1; }
# A picture for the eye, read from the Venus head through QEMU's VNC.
shot() { timeout 60 python3 plan/ws035/tests/zdesktop-check.py "$out/$1.png" --runtime "$GUEST_RUNTIME" >> "$out/qmp.txt" 2>&1; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$qmp" "$@" >/dev/null; }
key() { send input-send-event "{\"events\":[{\"type\":\"key\",\"data\":{\"down\":$2,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}]}"; }
tap() { key "$1" true; sleep 0.12; key "$1" false; sleep 0.6; }
count() { guest "grep -c -- '$1' /tmp/zdesktop.log" | tail -1; }
last() { guest "grep -- '$1' /tmp/zdesktop.log | tail -1"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[w]ltest|[t]ouchinject" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland( |$)|[w]ltest" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
env='export XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-0;'
status=0
pass() { echo "$1: ok"; }
fail() { echo "$1: FAILED"; status=1; }
expect_count() { n=$(count "$2"); if [ "${n:-0}" -eq "$3" ] 2>/dev/null; then pass "$1"; else fail "$1 ($2: ${n:-?}, expected $3)"; fi; }
expect_some() { n=$(count "$2"); if [ "${n:-0}" -ge 1 ] 2>/dev/null; then pass "$1"; else fail "$1 ($2: none)"; fi; }
expect_more() { n=$(count "$2"); if [ "${n:-0}" -gt "$3" ] 2>/dev/null; then pass "$1"; else fail "$1 ($2: ${n:-?}, expected more than $3)"; fi; }
# Each wltest's pid is kept in /tmp/NAME.pid when it starts (ps cannot tell two wltests apart).
open_app() { guest "$env /bin/wltest --app-id=$1 --windowed --size=$3 --color=$2 --frames=3600 --delay-ms=250 > /tmp/$1.log 2>&1 </dev/null & echo \$! > /tmp/$1.pid; sleep 3; echo started" >/dev/null; }
# fits_slot CLIENT: the client's latest image (KWL GLASS resized), drawn after the latest arrangement, has the size of its latest configure.
fits_slot() {
	applied=$(guest "grep -n 'KWL ARRANGE apply ' /tmp/zdesktop.log | tail -1" | sed -n 's/^\([0-9]*\):.*/\1/p')
	configured=$(guest "grep 'KWL CONFIGURE client=$1 ' /tmp/zdesktop.log | tail -1" | sed -n 's/.* width=\([0-9]*\) height=\([0-9]*\) .*/\1x\2/p')
	resized=$(guest "grep -n 'KWL GLASS resized .*client=$1\$' /tmp/zdesktop.log | tail -1")
	at=$(echo "$resized" | sed -n 's/^\([0-9]*\):.*/\1/p')
	drawn=$(echo "$resized" | sed -n 's/.* width=\([0-9]*\) height=\([0-9]*\) .*/\1x\2/p')
	[ -n "$at" ] && [ -n "$applied" ] && [ "$at" -gt "$applied" ] && [ -n "$drawn" ] && [ "$drawn" = "$configured" ]
}
client_of() { guest "grep -n 'KWL MAP client=' /tmp/zdesktop.log | tail -1" | sed -n 's/.*client=\([0-9]*\) .*/\1/p'; }
# Where a client's window floats now: its latest MAP, float-quiet or moved place (x y of the body).
place_of() {
	guest "grep -E 'KWL (MAP client=$1 |LAYOUT float-quiet .*client=$1\$|GLASS moved .*client=$1\$|GLASS undock .*client=$1\$)' /tmp/zdesktop.log | tail -1" |
	    sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p'
}
# A double click on the floating title bar of a client's window.
title_double_click() {
	set -- $(place_of "$1")
	pointer move $((${1:-300} + 120)) $((${2:-300} - 30)) sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 1500
}
# A double click on the docked window's title in the system bar (its x from the latest dock line).
bar_double_click() {
	set -- $(last 'KWL GLASS dock surface=' | sed -n 's/.* title=\([0-9]*\).*/\1/p')
	pointer move $((${1:-200} + 60)) 30 sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 1500
}
# Prints the pointer steps of a stroke: a press at (x0, y0), n moves step_ms apart to (x1, y1); no release.
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
# Replays a touch script (frames separated by '|') on the output's pixels.
touches() {
	printf 'size 1279 799 2\nwait 2600\n%s\nhold 300\n' "$1" | tr '|' '\n' > "$out/touch.script"
	put "$out/touch.script" /tmp/touch.script
	guest '/bin/touchinject /tmp/touch.script; echo replay=$?' | grep -q '^replay=0$' || fail touchinject
	sleep 0.8
}
: > "$out/qmp.txt"

# The compositor and wltest under test, and three windows.
guest "$stop_all" >/dev/null
put "$build/bin/wayland" /bin/wayland
put "$build/bin/wltest" /bin/wltest
guest 'chmod 755 /bin/wayland /bin/wltest; export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass > /tmp/zdesktop.log 2>&1 </dev/null & for w in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do grep -q KWL.READY /tmp/zdesktop.log 2>/dev/null && break; sleep 0.5; done; sleep 2; echo started' >/dev/null
open_app apps.a f4d0d0 380x260
a=$(client_of)
open_app apps.b d0f4d0 400x280
b=$(client_of)
echo "apps.a client ${a:-?}, apps.b client ${b:-?}"
pointer move 640 600 sleep 300

# A1. b docked, Alt+Tab to a (docked too), a brought back from the bar: every window floats.
title_double_click "${b:-0}"
expect_some docked 'KWL LAYOUT mode=docked reason=double-click'
key alt true
tap tab
tap tab
key alt false
sleep 1.5
expect_some switch-docks "KWL LAYOUT switch surface=[0-9]* action=dock mode=docked via=switch client=${a:-0}\$"
bar_double_click
expect_some leave-all-float 'KWL LAYOUT leave via=double-click front=[0-9]* quiet=1'
expect_some behind-floats "KWL LAYOUT float-quiet surface=[0-9]* .*client=${b:-0}\$"
expect_some none-docked 'KWL LAYOUT windows desktop=2 mode=windowed floating=2 docked=0'
shot a1-windowed

# A2. A click on b's body: it comes forward, its size unchanged.
undocks=$(count 'KWL GLASS undock')
set -- $(place_of "${b:-0}")
pointer move $((${1:-300} + 100)) $((${2:-300} + 100)) sleep 300 down sleep 60 up sleep 800
expect_count click-no-undock 'KWL GLASS undock' "${undocks:-0}"
expect_count click-no-float 'KWL LAYOUT switch surface=[0-9]* action=float' 0

# A3. a docked, then a ends: the docked mode ends, b stays a window.
title_double_click "${a:-0}"
guest 'kill $(cat /tmp/apps.a.pid); sleep 2; echo killed' >/dev/null
expect_some closed-leaves 'KWL LAYOUT leave via=closed front=0'
expect_count front-not-docked 'KWL LAYOUT front surface=[0-9]* action=dock' 0

# A4. b docked; c opens docked in the docked mode and owns the desktop, still docked 1.5 s later.
title_double_click "${b:-0}"
open_app apps.c d0d0f4 360x240
c=$(client_of)
sleep 1.5
leaves=$(count 'KWL LAYOUT leave ')
expect_some c-open-docked "KWL GLASS open-docked client=${c:-0} "
expect_some c-owner "KWL LAYOUT owner desktop=2 surface=[0-9]* client=${c:-0}\$"
sleep 1.5
expect_count c-still-docked 'KWL LAYOUT leave ' "${leaves:-0}"

# A5. c carried to the right desktop and back: still docked.
key ctrl true; key alt true; key shift true
tap right
key shift false; key alt false; key ctrl false
sleep 1.5
expect_some carried 'KWL LAYOUT owner desktop=3 surface=[0-9]* carried'
expect_count carried-no-leave 'KWL LAYOUT leave ' "${leaves:-0}"
key ctrl true; key alt true; key shift true
tap left
key shift false; key alt false; key ctrl false
sleep 1.5

# A6. c killed with -9: the docked mode ends.
guest 'kill -9 $(cat /tmp/apps.c.pid); sleep 2; echo killed' >/dev/null
expect_more killed-leaves 'KWL LAYOUT leave via=closed front=0' 1
shot a6-after-kill

# B7. Home from the bottom edge (mouse), held half way for the eye, its bottom edge does nothing, a drag down closes it.
pointer $(stroke 640 796 640 616 6 30) sleep 400
shot b7-home-half
pointer move 640 486 sleep 30 move 640 356 sleep 30 up sleep 1500
expect_some home-edge 'KWL HOME open via=edge'
shot b7-home-edge
closes=$(count 'KWL HOME close via=')
pointer $(stroke 640 796 640 556 8 40) up sleep 1500
expect_count home-bottom-nothing 'KWL HOME close via=' "${closes:-0}"
pointer $(stroke 640 300 640 600 10 30) up sleep 1500
expect_some home-pull-down 'KWL HOME close via=pull-down'

# B7b. The top-left corner's drag opens nothing (WS184's); its click opens Home, a second click closes it.
opens=$(count 'KWL HOME open via=')
pointer $(stroke 8 8 300 300 10 30) up sleep 1000
expect_some corner-slipped 'KWL HOME press slipped'
expect_count corner-drag-no-home 'KWL HOME open via=' "${opens:-0}"
pointer move 8 8 sleep 300 down sleep 60 up sleep 1200
expect_some corner-click-opens 'KWL HOME open via=launcher'
pointer move 8 8 sleep 300 down sleep 60 up sleep 1200
expect_some corner-click-closes 'KWL HOME close via=launcher'

# B8. Wiseview from the top band (a finger), closed by a tap; a tap on the clock in the band reaches it.
touches "down 1 640 4|swipe 0 200 8 30|up 1"
sleep 1
expect_some wiseview-top 'KWL WISEVIEW gesture via=top-edge'
expect_some wiseview-opens 'KWL WISEVIEW opening'
shot b8-wiseview-top
touches "down 1 60 120|hold 60|up 1"
expect_some wiseview-closed 'KWL WISEVIEW closed'
set -- $(last 'KWL GLASS desktops x=' | sed -n 's/.* x=\([0-9]*\) .*/\1/p')
menus=$(count 'KWL ARRANGE menu open')
touches "down 1 $((${1:-560} + 40)) 5|hold 60|up 1"
expect_some band-replay 'KWL EDGE band replay release=1'
expect_count band-menu 'KWL ARRANGE menu open' $((${menus:-0} + 1))
tap esc
expect_some menu-esc 'KWL ARRANGE menu close via=key'

# B9. b docked, its title pulled sideways in the bar and let go there: it comes off, moves, not docked again.  While
# it is docked the desktops' pill has moved from the middle to left of the status (ws181-p011): its first slot past 640.
title_double_click "${b:-0}"
sleep 1
set -- $(last 'KWL GLASS desktops x=' | sed -n 's/.* x=\([0-9]*\) .*/\1/p')
if [ "${1:-0}" -gt 640 ] 2>/dev/null; then pass "pill-docked-right (${1:-0})"; else fail "pill-docked-right (x=${1:-0}, expected past 640)"; fi
drags=$(count 'KWL GLASS dock surface=[0-9]* via=drag')
set -- $(last 'KWL GLASS dock surface=' | sed -n 's/.* title=\([0-9]*\).*/\1/p')
pointer $(stroke $((${1:-200} + 60)) 30 $((${1:-200} + 260)) 30 10 30) up sleep 1500
expect_some pull-leaves 'KWL LAYOUT leave via=pull'
expect_some pull-moves "KWL GLASS moved surface=[0-9]* .*client=${b:-0}\$"
expect_count pull-not-redocked 'KWL GLASS dock surface=[0-9]* via=drag' "${drags:-0}"

# C10. Three windows; the pill opens the menu; One on the Right arranges them.
open_app apps.d f4f0c0 360x240
d=$(client_of)
open_app apps.e c0f0f4 300x220
e=$(client_of)
set -- $(last 'KWL GLASS desktops x=' | sed -n 's/.* x=\([0-9]*\) step=\([0-9]*\) .*/\1 \2/p')
pointer move $((${1:-560} + 40)) 22 sleep 300 down sleep 60 up
shot c10-menu-opening
expect_some menu-open 'KWL ARRANGE menu open'
sleep 0.3
expect_some menu-settled 'KWL ARRANGE menu settled'
shot c10-arrange-menu
set -- $(last 'KWL ARRANGE menu item=right-main ' | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\).*/\1 \2/p')
glides=$(count 'KWL ARRANGE glide-end')
pointer move ${1:-640} ${2:-200} sleep 300 down sleep 60 up sleep 1200
expect_some arranged 'KWL ARRANGE apply layout=right-main desktop=2 windows=3'
# The picture is taken once the three windows' glides (180 ms) are over, and their clients have drawn the slot's size.
i=0; while [ "$(count 'KWL ARRANGE glide-end')" -lt $((${glides:-0} + 3)) ] 2>/dev/null && [ $i -lt 10 ]; do sleep 0.3; i=$((i+1)); done
i=0; while [ $i -lt 20 ]; do fits_slot "${b:-0}" && fits_slot "${d:-0}" && fits_slot "${e:-0}" && break; sleep 0.4; i=$((i+1)); done
for c in "${b:-0}" "${d:-0}" "${e:-0}"; do fits_slot "$c" && pass "arranged-size-$c" || fail "arranged-size-$c (its image is not the size of its slot)"; done
sleep 0.3
shot c10-arranged

# C11. The window of slot 1 (left top) dragged by its title onto slot 0 (the right one): a swap.
line=$(last 'KWL ARRANGE apply layout=right-main')
s0=$(echo "$line" | sed -n 's/.*slots=[0-9]*@\([0-9]*\),\([0-9]*\),\([0-9]*\),\([0-9]*\).*/\1 \2 \3 \4/p')
s1=$(echo "$line" | sed -n 's/.*slots=[^;]*;[0-9]*@\([0-9]*\),\([0-9]*\),\([0-9]*\),\([0-9]*\).*/\1 \2 \3 \4/p')
set -- $s1
fx=$((${1:-20} + 60)); fy=$((${2:-60} + 22))
set -- $s0
tx=$((${1:-660} + ${3:-600} / 2)); ty=$((${2:-60} + ${4:-700} / 2))
pointer $(stroke $fx $fy $tx $ty 12 30) up sleep 1200
expect_some swapped 'KWL ARRANGE swap a=[0-9]* b=[0-9]* slots=1,0'
shot c11-swapped

# C12. A double click on the right slot's title docks it, the mode ends; brought back, a title drag is a move.
set -- $s0
pointer move $((${1:-660} + 60)) $((${2:-60} + 22)) sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 1500
expect_some arrange-end-dock 'KWL ARRANGE end desktop=2 reason=dock'
bar_double_click
expect_some arranged-floats 'KWL LAYOUT windows desktop=2 mode=windowed .* docked=0'
swaps=$(count 'KWL ARRANGE swap-start')
moves=$(count 'KWL GLASS moved')
set -- $s1
pointer $(stroke $((${1:-20} + 60)) $((${2:-60} + 22)) $((${1:-20} + 160)) $((${2:-60} + 122)) 8 30) up sleep 1000
expect_count no-swap-after 'KWL ARRANGE swap-start' "${swaps:-0}"
expect_more moved-after 'KWL GLASS moved' "${moves:-0}"

# 13. Up, without errors.
running=$(guest 'ps -A -o args | grep -cE "[w]ayland( |$)"' | tail -1)
[ "$running" = "1" ] && pass alive || fail alive
guest 'grep -E "KWL (LAYOUT|ARRANGE|HOME (open|close|rise|press)|WISEVIEW|EDGE)|KWL GLASS (dock|undock|moved|move-desktop)|KWL MAP|ERROR" /tmp/zdesktop.log' > "$out/log.txt"
# The whole log and the clients' sizes, for a reader of a failure.
guest 'cat /tmp/zdesktop.log' > "$out/zdesktop.log"
guest 'grep -h "WLTEST RESIZE" /tmp/apps.*.log' > "$out/wltest-resize.log"
grep -q ERROR "$out/log.txt" && fail no-error || pass no-error
guest "$stop_all" >/dev/null

echo "ws181-guest: status $status (outputs in $out; a1-windowed, a6-after-kill, b7-home-half, b7-home-edge, b8-wiseview-top, c10-menu-opening, c10-arrange-menu, c10-arranged, c11-swapped .png for the eye)"
exit $status
