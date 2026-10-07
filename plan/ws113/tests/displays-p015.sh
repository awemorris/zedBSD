#!/bin/sh
# ws113-p015 (each display's own bar and states, App Home's background on the heads) on the Venus guest with two heads
# through QEMU's D-Bus display.  The image is plan/ws113/tests/config-amd64-p015.mk's (the p006 image with wltest),
# started with
#   VENUS_DISPLAY=dbus VENUS_OUTPUTS=2 plan/tools/files/files-guest.sh start IMAGE
# Head 0 is 1280x800 (the anchor), head 1 1024x768 right of it.  QEMU's tablet is absolute and maps onto the anchor, so
# the pointer works on the anchor only here: a window goes to head 1 by Super+Shift+Right, and a press on head 1 (its
# bar, a frame's resize) is the hardware's (the 5330, the user).  The judgement is by the compositor's lines and, for
# the eye, the heads' pictures (qmp-head-shot.py).
#  1. Window a opens on the anchor and goes to head 1 (why=key): its body is placed under head 1's own bar and a title
#     bar (y >= 96).  Head 1's bar is the system bar's kind (the 2026-10-08 user decision): its parts are logged
#     ("KWL GLASS head bar output=1 top=T launcher=X desktops=X status=X clock=X", left to right inside head 1) and
#     it has a's icon alone ("KWL APPS bar count=1 hidden=0 desktop=D apps=p015.a output=1"); head1-bar.png shows
#     across head 1's top the launcher, a's icon, the desktops' pill, the status and the clock, and a under it.
#  2. Window b opens on the anchor, and the system bar has b's icon alone ("KWL APPS bar count=1 hidden=0 desktop=D
#     apps=p015.b": each display's bar has its own windows, the 2026-10-08 user decision); a double click on its title
#     docks it there ("KWL GLASS dock surface=b
#     via=double-click"): the anchor's docked mode ("KWL LAYOUT mode=docked reason=double-click at_ms=", no output=),
#     whose window count is the anchor's alone ("KWL LAYOUT windows ... docked=1 dock_hidden=0" without output=: a on
#     head 1 is not hidden by it), and a stays in head 1's render list (head1-anchor-docked.png shows a floating there).
#     A double click on b's title in the system bar brings it back ("KWL LAYOUT leave via=double-click").
#  3. The arrangement menu on the anchor's pill, Side by Side: it arranges the anchor's window b alone ("KWL ARRANGE
#     apply layout=columns desktop=D windows=1"; the session's desktop is not always 1, and the slots' list on that line
#     is broken by the configure lines printed while it is written), a stays where it was on head 1.
#  4. App Home opened by a click in the top-left corner: head 1 shows App Home's dark stage alone, without a
#     (home-head1.png; home-anchor.png for the anchor); Esc closes it and head 1 shows a again (home-closed-head1.png).
#  5. Head 1 unplugged: a comes back to the anchor ("why=retreat").
#  6. No KWL FAILED; the compositor runs on.
# PASS: every "ok" line and the last line displays-p015: PASS.  The PNGs are for the eye (path to Q1).
#   plan/ws113/tests/displays-p015.sh [OUTDIR]     (default build/ws113-p015-guest)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws071-run}"
export GUEST_RUNTIME
out=${1:-build/ws113-p015-guest}
mkdir -p "$out"
qmp="$GUEST_RUNTIME/qmp.sock"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null | tr -d '\r'; }
head_set() { sh plan/tools/guest/venus-head.sh "$1" "$2"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$qmp" "$@"; sleep 1; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$qmp" "$@" >/dev/null; }
head_shot() { timeout 60 python3 plan/ws113/tests/qmp-head-shot.py "$qmp" "$1" "$out/$2.png" >> "$out/shots.txt" 2>&1; }
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[d]isplay-events|[w]ltest|[f]iles" | awk "{print \$1}"); do kill $p; done; sleep 1'
start_compositor='export XDG_RUNTIME_DIR=/tmp HOME=/tmp/p015-home; rm -f /tmp/wayland-0
picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
nohup /bin/wayland --testing --timeout=600 --glass $picture > /tmp/zdesktop.log 2>&1 </dev/null & echo started'
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

# Counts the compositor's lines matching a pattern, and gives the last of them.
count_lines() {
	guest "grep -cE '$1' /tmp/zdesktop.log" | tail -1
}
last_line() {
	guest "grep -E '$1' /tmp/zdesktop.log | tail -1" | tail -1
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

# Tells whether output N's last render list has a surface (yes or no).
listed() {
	line=$(last_line "KWL RENDER output=$1 ")
	case ",$(printf '%s' "$line" | sed -n 's/.*surfaces=//p' | tr -d ' ')," in
	*",$2,"*) echo yes ;;
	*) echo no ;;
	esac
}

# Opens a wltest window (app id, colour, size) and gives its surface and body place from the newest KWL MAP line.
open_window() {
	maps=$(count_lines 'KWL MAP client=')
	guest "export XDG_RUNTIME_DIR=/tmp WAYLAND_DISPLAY=wayland-0 HOME=/tmp/p015-home
nohup /bin/wltest --app-id=$1 --windowed --size=$3 --color=$2 --frames=3600 --delay-ms=250 > /tmp/$1.log 2>&1 </dev/null & echo started" >/dev/null
	expect_count 'KWL MAP client=' $((${maps:-0} + 1)) "window $1 maps"
	set -- $(last_line 'KWL MAP client=' | sed -n 's/.* surface=\([0-9]*\) x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2 \3/p')
	msurface=${1:-}; mx=${2:-0}; my=${3:-0}
}

# The guest, the heads and the compositor.
wait_guest
guest "$stop_all" >/dev/null
guest 'rm -rf /tmp/p015-home; mkdir -p /tmp/p015-home' >/dev/null
head_set 0 1280x800
head_set 1 1024x768
sleep 3
guest "$start_compositor" >/dev/null
wait_line /tmp/zdesktop.log 'KWL OUTPUT head open name=Venus virtual display 1 ' 60
wait_desktop
pointer move 640 600 sleep 300

# 1. Window a to head 1, under its bar.
open_window p015.a f4d0d0 380x260
a=$msurface
echo "a: surface ${a:-none} at $mx,$my"
keys '<super-shift-right>'
expect_count "KWL WINDOW output surface=$a .*output=1 .*why=key" 1 "Super+Shift+Right gives a to head 1"
set -- $(last_line "KWL WINDOW output surface=$a .*output=1 .*why=key" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\) why=.*/\1 \2/p')
if [ "${2:-0}" -ge 96 ] 2>/dev/null; then echo "ok: a is under head 1's bar and a title bar (y=$2)"; else echo "FAIL: a's place on head 1 (y=${2:-?}, want >= 96)"; status=1; fi
expect_count 'KWL GLASS head bar output=1 ' 1 "head 1 draws its own bar"
set -- $(last_line 'KWL GLASS head bar output=1 ' | sed -n 's/.* launcher=\([-0-9]*\) desktops=\([-0-9]*\) status=\([-0-9]*\) clock=\([-0-9]*\).*/\1 \2 \3 \4/p')
# The launcher is 10 in from head 1's left, which is right of the anchor (x >= 1280); the clock's pill ends inside its 1024.
if [ "${1:-0}" -ge 1290 ] && [ "${2:-0}" -gt "${1:-0}" ] && [ "${3:-0}" -gt "${2:-0}" ] && [ "${4:-0}" -gt "${3:-0}" ] &&
	[ "${4:-0}" -lt $((${1:-0} - 10 + 1024)) ] 2>/dev/null; then
	echo "ok: head 1's launcher, desktops, status and clock lie left to right inside it (${1:-?} ${2:-?} ${3:-?} ${4:-?})"
else
	echo "FAIL: head 1's bar's parts (launcher=${1:-?} desktops=${2:-?} status=${3:-?} clock=${4:-?})"; status=1
fi
expect_count 'KWL APPS bar count=1 hidden=0 desktop=[0-9]+ apps=p015.a output=1$' 1 "head 1's bar has a's icon alone"
sleep 1
head_shot 1 head1-bar

# 2. Window b docked on the anchor: the anchor's docked mode alone.
open_window p015.b d0f4d0 400x280
b=$msurface
echo "b: surface ${b:-none} at $mx,$my"
expect_count 'KWL APPS bar count=1 hidden=0 desktop=[0-9]+ apps=p015.b$' 1 "the system bar has b's icon alone (a is head 1's)"
pointer move $((mx + 150)) $((my - 30)) sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 800
expect_count "KWL GLASS dock surface=$b via=double-click" 1 "a double click docks b on the anchor"
expect_count 'KWL LAYOUT mode=docked reason=double-click at_ms=' 1 "the anchor's mode is docked (its own line)"
line=$(last_line 'KWL LAYOUT windows desktop=[0-9]+ mode=docked ')
printf '%s\n' "$line" > "$out/layout-docked.txt"
expect_text "$line" 'floating=0 docked=1 dock_hidden=0 minimized=0 fullscreen=0$' "the anchor's count has b docked and nothing hidden (a is head 1's)"
if [ "$(listed 1 "$a")" = yes ]; then echo "ok: a stays in head 1's render list"; else echo "FAIL: a is not in head 1's render list"; status=1; fi
head_shot 1 head1-anchor-docked
head_shot 0 anchor-docked
set -- $(last_line "GLASS dock surface=$b via=double-click" | sed -n 's/.* buttons=\([0-9]*\),\([0-9]*\),\([0-9]*\) title=\([0-9]*\).*/\1 \2 \3 \4/p')
title_x=${4:-60}
pointer move 640 600 sleep 500
pointer move $((title_x + 60)) 17 sleep 400 down sleep 60 up sleep 60 down sleep 60 up sleep 800
expect_count 'KWL LAYOUT leave via=double-click front=' 1 "a double click on b's title in the bar brings it back"
pointer move 640 600 sleep 500

# 3. The anchor's arrangement: b alone.
set -- $(last_line 'KWL GLASS desktops x=' | sed -n 's/.* x=\([0-9]*\) .*/\1/p')
pill=${1:-900}
pointer move $((pill + 20)) 22 sleep 300 down sleep 60 up sleep 800
expect_count 'KWL ARRANGE menu open' 1 "the arrangement menu opens"
set -- $(last_line 'KWL ARRANGE menu item=columns ' | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\).*/\1 \2/p')
pointer move "${1:-0}" "${2:-0}" sleep 300 down sleep 60 up sleep 1500
expect_count 'KWL ARRANGE apply layout=columns desktop=[0-9]+ windows=1 ' 1 "Side by Side arranges the anchor's window alone (b; a is head 1's)"
if [ "$(listed 1 "$a")" = yes ]; then echo "ok: a stays on head 1"; else echo "FAIL: a left head 1"; status=1; fi
head_shot 0 anchor-arranged

# 4. App Home: head 1 shows its stage alone.
opened=$(count_lines 'KWL HOME opened apps=')
pointer move 8 8 sleep 300 down sleep 60 up sleep 1500
expect_count 'KWL HOME opened apps=' $((${opened:-0} + 1)) "App Home opens"
sleep 1
head_shot 1 home-head1
head_shot 0 home-anchor
keys '<esc>'
expect_count 'KWL HOME close' 1 "Esc closes App Home"
sleep 1
head_shot 1 home-closed-head1

# 5. Head 1 unplugged: a comes back to the anchor.
head_set 1 off
expect_count "KWL WINDOW output surface=$a .*output=0 .*why=retreat" 1 "the unplugging brings a to the anchor"

# 6. Nothing failed.
guest 'cat /tmp/zdesktop.log' > "$out/zdesktop.log"
if grep -q 'KWL FAILED' "$out/zdesktop.log"; then echo "FAIL: the compositor failed"; status=1; else echo "ok: no KWL FAILED"; fi
alive=$(guest 'ps -A -o args | grep -cE "^/bin/wayland( |$)"' | tail -1)
if [ "${alive:-0}" -gt 0 ] 2>/dev/null; then echo "ok: the compositor runs on"; else echo "FAIL: the compositor is gone"; status=1; fi
head_set 1 1024x768
guest "$stop_all" >/dev/null

[ $status = 0 ] && echo "displays-p015: PASS" || echo "displays-p015: FAIL"
exit $status
