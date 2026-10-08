#!/bin/sh
# ws181-p009 (the 2026-10-07 UAT on the 5320): on the pen test guest (plan/ws079/tests/config-amd64-pen.mk;
# plan/ws079/tests/pen-guest.sh start IMAGE), the compositor and wltest under test (BUILD/bin/wayland,
# BUILD/bin/wltest) copied in, 1280x800, two wltest windows a and b.  The compositor goes to /bin/wayland-p009, a
# name no other process runs (T1-361: the copy over /bin/wayland did not take while the image's own compositor ran it,
# and the image's old compositor was tested); both copies are checked with cksum before anything runs, and the
# processes before and after the stop are kept in OUTDIR/ps.txt.  The mouse and keys through QMP, the touch pad
# through touchinject's "pad" scripts (1336x760 units at 12 a millimetre; each waits 2.6 s for the compositor's scan).
# A guest without /bin/calendar is given one for the run: a script that opens a wltest window (app id calendar).
#  7. The desktops' pill is in the middle of the bar while no window is docked (ws181-p011, the 2026-10-08 UAT; its first
#     slot "KWL GLASS desktops x=" 591 on 1280: the pill 118 wide from 581); p009-bar.png.  (Docked, it is left of the
#     status: ws181-guest.sh B9.)
#  1. The arrangement menu: the pointer on "rows" lights it ("KWL ARRANGE menu lit item=rows"); moved on into the gap
#     between "columns" and "rows", "columns" is not lit (no "lit item=columns"); p009-menu-gap.png shows "rows" lit.
#     Esc closes the menu; "Side by Side" (columns) arranges a and b ("KWL ARRANGE apply layout=columns ... windows=2").
#  6. On the arranged desktop a click on the clock opens Calendar ("KWL HOME open name=Calendar via=clock"), and its
#     window joins the arrangement ("KWL ARRANGE join surface=N via=mapped", then "apply layout=columns ... windows=3");
#     a second click (Calendar runs) arranges it again ("KWL ARRANGE join via=running"); p009-calendar-arranged.png.
#  5. On App Home (a click in the top-left corner) a click on the clock opens nothing (no more "open name=Calendar").
#  4. On App Home's first page a drag to the right, held: no page slides in from the left (p009-home-first-drag.png
#     shows the first page where it was); let go, the first page stays ("KWL HOME page page=1 ... via=drag").  When
#     Home has more than one page, the same to the left on the last page (p009-home-last-drag.png).
#  3. On App Home, two fingers up 30 mm from the pad's bottom edge close it following them ("KWL HOME pad swipe
#     closing=1", "KWL HOME close via=pad"); the same with one finger only in the band too; a short slow one leaves Home
#     open ("KWL HOME pad stays").
#  The compositor stays up, without KWL FAILED.
#
#   plan/ws181/tests/p009-guest.sh BUILD [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws079-run}"
build=${1:?usage: p009-guest.sh BUILD [OUTDIR]}
out=${2:-build/ws181-p009-guest}
mkdir -p "$out"
qmp="$GUEST_RUNTIME/qmp.sock"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
put() { timeout 90 python3 plan/tools/guest/guest.py put "$1" "$2" >> "$out/put.txt" 2>&1; }
# Copies a file in and proves the guest's copy is the same bytes (cksum), or says why not.
install_file() {
	want=$(cksum < "$1" | awk '{print $1, $2}')
	put "$1" "$2"
	got=$(guest "cksum < $2" | tail -1 | awk '{print $1, $2}')
	[ "$got" = "$want" ] && return 0
	echo "install_file $2: guest cksum '$got', host '$want'" >> "$out/put.txt"
	return 1
}
send() { timeout 40 python3 plan/ws049/tests/qmp-send.py "$qmp" "$@" >> "$out/qmp.txt" 2>&1; }
# A picture for the eye, read from the Venus head through QEMU's VNC.
shot() { timeout 60 python3 plan/ws035/tests/zdesktop-check.py "$out/$1.png" --runtime "$GUEST_RUNTIME" >> "$out/qmp.txt" 2>&1; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$qmp" "$@" >/dev/null; }
key() { send input-send-event "{\"events\":[{\"type\":\"key\",\"data\":{\"down\":$2,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}]}"; }
tap() { key "$1" true; sleep 0.12; key "$1" false; sleep 0.6; }
count() { guest "grep -c -- '$1' /tmp/zdesktop.log" | tail -1; }
last() { guest "grep -- '$1' /tmp/zdesktop.log | tail -1"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[w]ayland(-p009)?( |$)|[w]ltest|[t]ouchinject" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland(-p009)?( |$)|[w]ltest" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
env='export XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-0;'
status=0
pass() { echo "$1: ok"; }
fail() { echo "$1: FAILED"; status=1; }
expect_count() { n=$(count "$2"); if [ "${n:-0}" -eq "$3" ] 2>/dev/null; then pass "$1"; else fail "$1 ($2: ${n:-?}, expected $3)"; fi; }
expect_some() { n=$(count "$2"); if [ "${n:-0}" -ge 1 ] 2>/dev/null; then pass "$1"; else fail "$1 ($2: none)"; fi; }
open_app() { guest "$env /bin/wltest --app-id=$1 --windowed --size=$3 --color=$2 --frames=3600 --delay-ms=250 > /tmp/$1.log 2>&1 </dev/null & sleep 3; echo started" >/dev/null; }
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
# A pad script after the declaration and the 2.6 s wait; its lines are joined by newlines.
pad() { name=$1; shift; script="pad 1336 760 5 scan\nwait 2600"; for line in "$@"; do script="$script\n$line"; done
	guest "printf '$script\nhold 800\n' | /bin/touchinject; echo replay=\$?" > "$out/$name.txt"
	grep -q '^replay=0$' "$out/$name.txt" || fail "$name replay"; sleep 0.5; }
# Opens App Home by a click in the top-left corner, and waits for it to be open.
home_open() {
	opened=$(count 'KWL HOME opened apps=')
	pointer move 8 8 sleep 300 down sleep 60 up sleep 1200
	i=0; while [ "$(count 'KWL HOME opened apps=')" -le "${opened:-0}" ] 2>/dev/null && [ $i -lt 10 ]; do sleep 0.3; i=$((i+1)); done
}
: > "$out/qmp.txt"

# The compositor and wltest under test, a Calendar for the clock, and two windows.
: > "$out/put.txt"
guest 'echo "before the stop:"; ps -A -o pid,args' > "$out/ps.txt"
guest "$stop_all" >/dev/null
guest 'echo "after the stop:"; ps -A -o pid,args' >> "$out/ps.txt"
if ! install_file "$build/bin/wayland" /bin/wayland-p009 || ! install_file "$build/bin/wltest" /bin/wltest; then
	echo "setup: the compositor or wltest under test is not in the guest ($out/put.txt, $out/ps.txt)"
	echo "ws181-p009: FAIL"
	exit 1
fi
guest '[ -x /bin/calendar ] || { printf "#!/bin/sh\nexec /bin/wltest --app-id=calendar --windowed --size=360x260 --color=e8e0f8 --frames=3600 --delay-ms=250\n" > /bin/calendar; chmod 755 /bin/calendar; echo calendar-script; }' > "$out/calendar.txt"
guest 'chmod 755 /bin/wayland-p009 /bin/wltest; export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
/bin/wayland-p009 --testing --timeout=900 --width=1280 --height=800 --glass > /tmp/zdesktop.log 2>&1 </dev/null & for w in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do grep -q KWL.READY /tmp/zdesktop.log 2>/dev/null && break; sleep 0.5; done; sleep 2; echo started' >/dev/null
open_app apps.a f4d0d0 380x260
open_app apps.b d0f4d0 400x280
pointer move 640 600 sleep 300

# 7. The desktops' pill in the middle of the bar (no window docked, ws181-p011).
set -- $(last 'KWL GLASS desktops x=' | sed -n 's/.* x=\([0-9]*\) .*/\1/p')
pill=${1:-0}
if [ "$pill" -ge 589 ] 2>/dev/null && [ "$pill" -le 593 ]; then pass "pill-middle ($pill)"; else fail "pill-middle (x=$pill, expected 591)"; fi
shot p009-bar

# 1. The menu: "rows" lit, then the gap: "columns" not lit; Esc; then Side by Side arranges a and b.
pointer move $((pill + 40)) 22 sleep 300 down sleep 60 up sleep 600
expect_some menu-open 'KWL ARRANGE menu open'
set -- $(last 'KWL ARRANGE menu item=columns ' | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\).*/\1 \2/p')
cx=${1:-0}; cy=${2:-0}
set -- $(last 'KWL ARRANGE menu item=rows ' | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\).*/\1 \2/p')
rx=${1:-0}; ry=${2:-0}
pointer move "$rx" "$ry" sleep 400 move $(((cx + rx) / 2)) "$ry" sleep 600
expect_count menu-lit-rows 'KWL ARRANGE menu lit item=rows' 1
expect_count menu-gap-not-first 'KWL ARRANGE menu lit item=columns' 0
shot p009-menu-gap
tap esc
expect_some menu-esc 'KWL ARRANGE menu close via=key'
sleep 0.5
pointer move $((pill + 40)) 22 sleep 300 down sleep 60 up sleep 600
pointer move "$cx" "$cy" sleep 300 down sleep 60 up sleep 1500
expect_some arranged 'KWL ARRANGE apply layout=columns desktop=2 windows=2'

# 6. The clock on the arranged desktop: Calendar joins the arrangement; a second click arranges it again.
pointer move 1250 22 sleep 300 down sleep 60 up
i=0; while [ "$(count 'KWL ARRANGE join surface=')" -lt 1 ] 2>/dev/null && [ $i -lt 20 ]; do sleep 0.5; i=$((i+1)); done
expect_some clock-calendar 'KWL HOME open name=Calendar via=clock'
expect_some calendar-joins 'KWL ARRANGE join surface=[0-9]* via=mapped'
expect_some calendar-arranged 'KWL ARRANGE apply layout=columns desktop=2 windows=3'
sleep 1
shot p009-calendar-arranged
pointer move 1250 22 sleep 300 down sleep 60 up sleep 1500
expect_some calendar-running-joins 'KWL ARRANGE join via=running'

# 5. On App Home the clock opens nothing.
home_open
expect_some home-open 'KWL HOME opened apps='
calendars=$(count 'KWL HOME open name=Calendar')
pointer move 1250 22 sleep 300 down sleep 60 up sleep 1200
expect_count home-clock-nothing 'KWL HOME open name=Calendar' "${calendars:-0}"

# 4. The first page dragged to the right, held, then let go; the last page to the left when there are pages.
pages=$(last 'KWL HOME opened apps=' | sed -n 's/.* pages=\([0-9]*\) .*/\1/p')
echo "home pages ${pages:-?}"
pointer $(stroke 500 420 900 420 10 30) sleep 300
shot p009-home-first-drag
pointer up sleep 1200
expect_some first-page-stays 'KWL HOME page page=1 pages=[0-9]* via=drag'
if [ "${pages:-1}" -gt 1 ] 2>/dev/null; then
	pointer $(stroke 900 420 500 420 10 30) up sleep 1200
	turned=$(count 'KWL HOME page page=2 pages=')
	n=2
	while [ "$n" -lt "$pages" ]; do
		pointer $(stroke 900 420 500 420 10 30) up sleep 1200
		n=$((n + 1))
	done
	pointer $(stroke 900 420 500 420 10 30) sleep 300
	shot p009-home-last-drag
	pointer up sleep 1200
	expect_some last-page-stays "KWL HOME page page=$pages pages=$pages via=drag"
	echo "turned to page 2: ${turned:-0}"
else
	echo "note: one page only, the last page's drag is not tried"
fi

# 3. Two fingers up from the pad's bottom edge close Home; one finger in the band is enough; a short slow one stays.
pad bottom2-home "down 0 500 750; down 1 700 745" "wait 30" "swipe 0 -360 12 16" "up 0; up 1"
expect_count pad-closing 'KWL HOME pad swipe closing=1' 1
expect_count pad-closes 'KWL HOME close via=pad' 1
sleep 1
home_open
pad bottom2-one "down 0 500 750; down 1 700 500" "wait 30" "swipe 0 -360 12 16" "up 0; up 1"
expect_count pad-one-closing 'KWL HOME pad swipe closing=1' 2
expect_count pad-one-closes 'KWL HOME close via=pad' 2
sleep 1
home_open
pad bottom2-short "down 0 500 750; down 1 700 745" "wait 30" "swipe 0 -96 12 60" "up 0; up 1"
expect_count pad-short-closing 'KWL HOME pad swipe closing=1' 3
expect_some pad-short-stays 'KWL HOME pad stays from='
expect_count pad-short-not-closed 'KWL HOME close via=pad' 2
tap esc

# The compositor stays up.
failed=$(count 'KWL FAILED')
[ "${failed:-1}" -eq 0 ] 2>/dev/null && pass no-failed || fail "no-failed (${failed:-?})"
alive=$(guest 'ps -A -o args | grep -cE "^/bin/wayland-p009( |$)"' | tail -1)
[ "${alive:-0}" -gt 0 ] 2>/dev/null && pass alive || fail alive
guest 'cat /tmp/zdesktop.log' > "$out/zdesktop.log"
guest "$stop_all" >/dev/null

[ $status = 0 ] && echo "ws181-p009: PASS" || echo "ws181-p009: FAIL"
exit $status
