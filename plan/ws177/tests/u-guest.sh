#!/bin/sh
# WS177 case U (ws177-p033 to p038; the edges, the arrangement and Home's bar of WS181 off the normal path), on the pen
# test guest (plan/ws079/tests/config-amd64-pen.mk; plan/ws079/tests/pen-guest.sh start IMAGE).  The compositor and
# wltest under test (BUILD/bin/wayland, BUILD/bin/wltest) are copied in; the compositor runs at 1280x800 on a white
# wallpaper (the white status over App Home is read against the brightest wallpaper).  The mouse and keys through QMP,
# the touch screen through touchinject.
#  U1 (p033). A finger held 900 ms on an application's icon in the top band: a long press ("KWL EDGE band hold ms=N
#     fullscreen=0"), and the icon shows its previews at once ("KWL APPS preview app=u.a windows=2 via=hold"),
#     u1-hold.png; they stay after the lift.  A short tap on it still shows them by a click ("band replay release=1",
#     "via=click").
#  U2 (p033, p038). App Home opened by the launcher: once open the desktop layer is not drawn ("KWL HOME layer
#     hidden=1"); a press between the status's icons (the status pill's left padding) and one on the clock are Home's
#     ("KWL HOME bar gap x=", twice) and open nothing ("open name=Calendar" none); u2-home-white.png shows the white
#     status on Home over the white wallpaper.  Closed, the layer is drawn again ("hidden=0").  The swipe up from the
#     bottom held half way: u2-bar-fade.png shows the bar half faded with the white status coming in.
#  U3 (p034). Over a fullscreen wltest: a finger in the top band that slips sideways, a tap there and a long press
#     there each go to the fullscreen client ("KWL EDGE band handback lifted=0 given=1", "lifted=1 given=1",
#     "band hold ... fullscreen=1"), each with "KWL TOUCH handback contact=".
#  U4 (p035). Three windows: b (any size), m (smallest size 700x500) and x (one size, 300x200); "Side by Side": m is
#     too large for a third of the width and stays out ("KWL ARRANGE too-large surface="), b and x are arranged
#     ("apply layout=columns desktop=2 windows=2"), x at its own size in its slot (no "end ... reason=resized" 2 s
#     later; u4-letterbox.png); the first slot starts at x 16 or more and the last ends at 1264 or less (off the
#     desktops' swipe strips).
#  U5 (p036). The menu open, a click on the volume's icon closes the menu first ("KWL ARRANGE menu close via=bar") and
#     opens the volume's popup ("KWL VOLUME popup open"); Esc.  A title's swap being dragged when Alt+Shift+Right
#     switches the desktop is given up ("KWL ARRANGE swap-cancel ... reason=desktop"); back with Alt+Shift+Left.
#  U6 (p037). Super with an arrow swaps the focused arranged window with its neighbour ("KWL ARRANGE key-swap a="); the
#     mode ended (a double click docks one, the bar's double click brings it back), "Side by Side" again puts the two
#     windows back in the slots they had after the key swap ("KWL ARRANGE recall desktop=2 layout=columns windows=2",
#     the apply line's slots in that order).
#  The compositor stays up, without ERROR.
#
#   plan/ws177/tests/u-guest.sh BUILD [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws079-run}"
build=${1:?usage: u-guest.sh BUILD [OUTDIR]}
out=${2:-build/ws177-u-guest}
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
# A wltest window (NAME COLOR SIZE [OPTIONS]); its pid in /tmp/NAME.pid.
open_app() { guest "$env /bin/wltest --app-id=$1 --windowed --size=$3 --color=$2 ${4:-} --frames=3600 --delay-ms=250 > /tmp/$1.log 2>&1 </dev/null & echo \$! > /tmp/$1.pid; sleep 3; echo started" >/dev/null; }
client_of() { guest "grep -n 'KWL MAP client=' /tmp/zdesktop.log | tail -1" | sed -n 's/.*client=\([0-9]*\) .*/\1/p'; }
# Where a client's window floats now: its latest MAP, float-quiet or moved place (x y of the body).
place_of() {
	guest "grep -E 'KWL (MAP client=$1 |LAYOUT float-quiet .*client=$1\$|GLASS moved .*client=$1\$|GLASS undock .*client=$1\$)' /tmp/zdesktop.log | tail -1" |
	    sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p'
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
# Opens the arrangement menu from the desktops' pill and chooses a layout (its middle from the menu's log).
arrange() {
	layout=$1
	set -- $(last 'KWL GLASS desktops x=' | sed -n 's/.* x=\([0-9]*\) step=\([0-9]*\) .*/\1 \2/p')
	pointer move $((${1:-560} + 40)) 22 sleep 300 down sleep 60 up sleep 500
	set -- $(last "KWL ARRANGE menu item=$layout " | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\).*/\1 \2/p')
	pointer move ${1:-640} ${2:-200} sleep 300 down sleep 60 up sleep 1500
}
# The surfaces of the latest apply line's slots, in the slots' order ("A;B").
slot_ids() { last 'KWL ARRANGE apply layout=' | sed -n 's/.*slots=//p' | sed 's/@[-0-9,]*//g'; }
: > "$out/qmp.txt"

# A white wallpaper (1280x800 PNG), the compositor and wltest under test.
python3 - "$out/white.png" <<'EOF'
import struct, sys, zlib
width, height = 1280, 800
row = b"\x00" + b"\xff\xff\xff" * width
def chunk(kind, data):
	return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(row * height, 9)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
EOF
guest "$stop_all" >/dev/null
put "$build/bin/wayland" /bin/wayland
put "$build/bin/wltest" /bin/wltest
put "$out/white.png" /tmp/white.png
guest 'chmod 755 /bin/wayland /bin/wltest; export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass --wallpaper=/tmp/white.png > /tmp/zdesktop.log 2>&1 </dev/null & for w in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do grep -q KWL.READY /tmp/zdesktop.log 2>/dev/null && break; sleep 0.5; done; sleep 2; echo started' >/dev/null

# U1. Two windows of one application; a finger's long press on its icon shows the previews at once.
open_app u.a f4d0d0 380x260
open_app u.a d0d0f4 360x240
pointer move 640 600 sleep 300
set -- $(last 'KWL APPS icon app=u.a ' | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
ix=$((${1:-60} + ${3:-30} / 2)); iy=$((${2:-6} + ${4:-30} / 2))
touches "down 1 $ix $iy|hold 900|up 1"
expect_some band-hold 'KWL EDGE band hold ms=[0-9]* fullscreen=0'
expect_some hold-preview 'KWL APPS preview app=u.a windows=2 via=hold'
shot u1-hold
tap esc
touches "down 1 $ix $iy|hold 60|up 1"
expect_some tap-replay 'KWL EDGE band replay release=1'
expect_some tap-preview 'KWL APPS preview app=u.a windows=2 via=click'
tap esc
guest 'for p in $(ps -A -o pid,args | grep "[w]ltest --app-id=u.a" | awk "{print \$1}"); do kill $p; done; sleep 1.5; echo killed' >/dev/null

# U2. Home: the layer left out once open, a press between the status's icons and on the clock are Home's.
pointer move 8 8 sleep 300 down sleep 60 up sleep 1500
expect_some home-open 'KWL HOME open via=launcher'
expect_some layer-hidden 'KWL HOME layer hidden=1'
shot u2-home-white
set -- $(last 'KWL GLASS status left=' | sed -n 's/.* left=\([0-9]*\) width=\([0-9]*\) clock=\([0-9]*\).*/\1 \2 \3/p')
calendars=$(count 'open name=Calendar')
pointer move $((${1:-900} + 3)) 22 sleep 300 down sleep 60 up sleep 600
expect_count status-gap 'KWL HOME bar gap x=' 1
pointer move $((${3:-1180} + 10)) 22 sleep 300 down sleep 60 up sleep 600
expect_count clock-is-home 'KWL HOME bar gap x=' 2
expect_count clock-no-calendar 'open name=Calendar' "${calendars:-0}"
pointer move 8 8 sleep 300 down sleep 60 up sleep 1500
expect_some home-closed 'KWL HOME close via=launcher'
expect_some layer-drawn 'KWL HOME layer hidden=0'
pointer $(stroke 640 796 640 616 6 30) sleep 400
shot u2-bar-fade
pointer move 640 486 sleep 30 move 640 356 sleep 30 up sleep 1500
pointer $(stroke 640 300 640 600 10 30) up sleep 1500

# U3. A fullscreen window: the band's press that is no swipe goes to its client (slip, tap, long press).
guest "$env /bin/wltest --app-id=u.f --color=c0f0c0 --frames=3600 --delay-ms=250 > /tmp/u.f.log 2>&1 </dev/null & echo \$! > /tmp/u.f.pid; sleep 3; echo started" >/dev/null
touches "down 1 640 5|swipe 40 0 4 30|up 1"
expect_some slip-handback 'KWL EDGE band handback lifted=0 given=1'
touches "down 1 640 5|hold 60|up 1"
expect_some tap-handback 'KWL EDGE band handback lifted=1 given=1'
touches "down 1 640 5|hold 900|up 1"
expect_some hold-fullscreen 'KWL EDGE band hold ms=[0-9]* fullscreen=1'
expect_more touch-handbacks 'KWL TOUCH handback contact=' 2
guest 'kill $(cat /tmp/u.f.pid); sleep 1.5; echo killed' >/dev/null

# U4. A window too large for its slot stays out, a window of one size sits in its slot.
open_app u.b d0f4d0 400x280
b=$(client_of)
open_app u.m f4f0c0 720x520 --min-size=700x500
open_app u.x c0f0f4 300x200 --fixed
pointer move 640 600 sleep 300
arrange columns
expect_some too-large 'KWL ARRANGE too-large surface=[0-9]* min=700x500'
expect_some arranged-two 'KWL ARRANGE apply layout=columns desktop=2 windows=2'
sleep 2
expect_count fixed-stays 'KWL ARRANGE end desktop=2 reason=resized' 0
shot u4-letterbox
line=$(last 'KWL ARRANGE apply layout=columns')
first=$(echo "$line" | sed -n 's/.*slots=[0-9]*@\([0-9]*\),.*/\1/p')
set -- $(echo "$line" | sed -n 's/.*;[0-9]*@\([0-9]*\),[0-9]*,\([0-9]*\),[0-9]*$/\1 \2/p')
if [ "${first:-0}" -ge 16 ] 2>/dev/null && [ $((${1:-2000} + ${2:-0})) -le 1264 ]; then pass "off-side-strips ($first; $1+$2)"; else fail "off-side-strips (first x=${first:-?}, last ${1:-?}+${2:-?})"; fi

# U5. The menu closes first for the volume's icon; a swap being dragged is given up by a desktop switch.
set -- $(last 'KWL GLASS desktops x=' | sed -n 's/.* x=\([0-9]*\) .*/\1/p')
pointer move $((${1:-560} + 40)) 22 sleep 300 down sleep 60 up sleep 500
set -- $(last 'KWL VOLUME icon x=' | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
pointer move $((${1:-1100} + ${3:-20} / 2)) $((${2:-10} + ${4:-20} / 2)) sleep 300 down sleep 60 up sleep 800
expect_some menu-closes-first 'KWL ARRANGE menu close via=bar'
expect_some volume-opens 'KWL VOLUME popup open'
tap esc
line=$(last 'KWL ARRANGE apply layout=columns')
set -- $(echo "$line" | sed -n 's/.*slots=[0-9]*@\([0-9]*\),\([0-9]*\),.*/\1 \2/p')
pointer $(stroke $((${1:-24} + 60)) $((${2:-60} + 22)) $((${1:-24} + 200)) $((${2:-60} + 160)) 6 30) sleep 300
key alt true; key shift true
tap right
key shift false; key alt false
pointer up sleep 1000
expect_some swap-cancel 'KWL ARRANGE swap-cancel surface=[0-9]* slot=[0-9]* reason=desktop'
key alt true; key shift true
tap left
key shift false; key alt false
sleep 1

# U6. Super with an arrow swaps; ended and arranged again, the windows go back to the slots they had.
before=$(slot_ids)
key meta_l true
tap right
key meta_l false
n=$(count 'KWL ARRANGE key-swap a=')
if [ "${n:-0}" -lt 1 ] 2>/dev/null; then
	key meta_l true
	tap left
	key meta_l false
fi
expect_some key-swap 'KWL ARRANGE key-swap a='
a1=$(echo "$before" | cut -d';' -f1); a2=$(echo "$before" | cut -d';' -f2)
after_swap="$a2;$a1"
set -- $(last 'KWL ARRANGE apply layout=columns' | sed -n 's/.*slots=[0-9]*@\([0-9]*\),\([0-9]*\),.*/\1 \2/p')
pointer move $((${1:-24} + 60)) $((${2:-60} + 22)) sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 1500
expect_some arrange-end-dock 'KWL ARRANGE end desktop=2 reason=dock'
bar_double_click
arrange columns
expect_some recalled 'KWL ARRANGE recall desktop=2 layout=columns windows=2'
again=$(slot_ids)
if [ -n "$again" ] && [ "$again" = "$after_swap" ]; then pass "recalled-order ($again)"; else fail "recalled-order (got '$again', expected '$after_swap', before the key swap '$before')"; fi

# Up, without errors.
running=$(guest 'ps -A -o args | grep -cE "[w]ayland( |$)"' | tail -1)
[ "$running" = "1" ] && pass alive || fail alive
guest 'grep -E "KWL (ARRANGE|HOME|EDGE|TOUCH handback|APPS preview|VOLUME popup|GLASS (status|desktop=))|ERROR" /tmp/zdesktop.log' > "$out/log.txt"
guest 'cat /tmp/zdesktop.log' > "$out/zdesktop.log"
grep -q ERROR "$out/log.txt" && fail no-error || pass no-error
guest "$stop_all" >/dev/null

echo "u-guest: status $status (outputs in $out; u1-hold, u2-home-white, u2-bar-fade, u4-letterbox .png for the eye)"
exit $status
