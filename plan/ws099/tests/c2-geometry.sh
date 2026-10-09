#!/bin/sh
# ws099-p001, C2: a window's move, its four corners' and four sides' resize, maximize and back, minimize and back
# each end at the intended place and size, on the Venus guest of the criteria image (kei's session at boot).
# Files is opened from App Home (KWL GLASS launch ... to=X,Y size=WxH; launch-late when its window came after
# the 5 s the grow waits for, ws099-p024 BUG-147), then:
#  1. corners and sides: each dragged STEP pixels outwards (ws035-p128's way): the size grows by STEP in the dragged
#     directions, the opposite corner or side stays (KWL RESIZE settled x= y= width= height=).
#  2. move: its title bar dragged by (MOVE_DX, MOVE_DY): KWL GLASS moved x= y= is the old place plus that.
#  3. maximize and back: a double click on the title bar docks it (KWL GLASS dock: the width less KWL_GLASS_DOCK_PAD on
#     each side, ws099-p038), a double click on its title in
#     the system bar brings it back (KWL GLASS undock x= y=) to the same place, and its next configure has the size
#     it had (KWL CONFIGURE ... width= height=).
#  4. minimize and back: its minimize button (KWL GLASS minimize), then Wiseview (Super+Tab) and a click on its tile
#     (KWL WISEVIEW select): a small title bar drag afterwards moves it from the place it had.
# Prints "C2 RESULT pass=N fail=M" and "C2: PASS" or "C2: FAIL".
#
#   VENUS_SIZE=1920x1280 plan/ws035/tests/zdesktop-guest.sh start build/ws099-criteria.img
#   VENUS_SIZE=1920x1280 plan/ws099/tests/c2-geometry.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws035-sq-run}"
export GUEST_RUNTIME
out=${1:-build/ws099-shots/c2}
step=${STEP:-40}
MOVE_DX=${MOVE_DX:-60}
MOVE_DY=${MOVE_DY:-40}
size=${VENUS_SIZE:-1920x1280}
mkdir -p "$out"
log=/run/user/1000/session.log
# The SSH to the guest, tried again when ssh itself fails (plan/ws099/tests/guest-retry.sh, ws099-p024).
. plan/ws099/tests/guest-retry.sh
guest() { guest_retry 120 "$1" </dev/null; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py --width "${size%x*}" --height "${size#*x}" "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; sleep 0.8; }
shot() { python3 plan/ws035/tests/zdesktop-check.py "$out/$1.png" --runtime "$GUEST_RUNTIME" >/dev/null 2>&1; }
pass=0
fail=0

# Counts a check.
verdict() {
	if [ "$1" = ok ]; then
		pass=$((pass + 1))
		echo "$2 ok"
	else
		fail=$((fail + 1))
		echo "$2 FAIL"
	fi
}

# The number of lines of the session's log matching a pattern.
count() {
	guest "grep -cE '$1' $log" | tail -1
}

# Waits until the session's log has more than BEFORE lines matching a pattern (within some seconds).
expect_more() {
	tries=0
	found=0
	while [ $tries -lt "$3" ]; do
		found=$(count "$1")
		[ "${found:-0}" -gt "$2" ] 2>/dev/null && break
		tries=$((tries + 1))
		sleep 1
	done
	if [ "${found:-0}" -gt "$2" ] 2>/dev/null; then
		return 0
	fi
	verdict no "log: $1"
	return 1
}

# The last line of the session's log matching a pattern.
last() {
	guest "grep -E '$1' $log | tail -1"
}

# Reads the window's outline (the body; its title bar is 52 pixels above it) from the last settled resize or move.
geometry() {
	line=$(last "KWL (RESIZE settled|GLASS moved|GLASS launch|GLASS launch-late) surface=$surface ")
	case $line in
	*launch*) set -- $(echo "$line" | sed -n 's/.* to=\(-*[0-9]*\),\(-*[0-9]*\) size=\([0-9]*\)x\([0-9]*\).*/\1 \2 \3 \4/p') ;;
	*moved*) set -- $(echo "$line" | sed -n 's/.* x=\(-*[0-9]*\) y=\(-*[0-9]*\).*/\1 \2/p') "$width" "$height" ;;
	*) set -- $(echo "$line" | sed -n 's/.* x=\(-*[0-9]*\) y=\(-*[0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p') ;;
	esac
	x=$1 y=$2 width=$3 height=$4
	left=$x right=$((x + width)) top=$((y - 52)) bottom=$((y + height))
}

# Drags one frame point by (DX, DY); checks the new size and that the opposite corner or side stayed.
drag() {
	name=$1 edges=$2 px=$3 py=$4 dx=$5 dy=$6 grow_w=$7 grow_h=$8
	settles=$(count "KWL RESIZE settled surface=$surface ")
	old_left=$left old_right=$right old_top=$top old_bottom=$bottom old_width=$width old_height=$height
	pointer move $((px - 6)) $((py - 6)) sleep 150 move "$px" "$py" sleep 300 down sleep 100 \
	    move $((px + dx / 2)) $((py + dy / 2)) sleep 100 move $((px + dx)) $((py + dy)) sleep 300 up sleep 200
	expect_more "KWL RESIZE settled surface=$surface " "$settles" 10 || return
	geometry
	echo "$name: ${old_width}x$old_height at $old_left,$old_top -> ${width}x$height at $left,$top"
	result=ok
	[ $((width - old_width)) -eq "$grow_w" ] && [ $((height - old_height)) -eq "$grow_h" ] || result=no
	case $edges in
	5) [ "$right" -eq "$old_right" ] && [ "$bottom" -eq "$old_bottom" ] || result=no ;;
	9) [ "$left" -eq "$old_left" ] && [ "$bottom" -eq "$old_bottom" ] || result=no ;;
	6) [ "$right" -eq "$old_right" ] && [ "$top" -eq "$old_top" ] || result=no ;;
	10) [ "$left" -eq "$old_left" ] && [ "$top" -eq "$old_top" ] || result=no ;;
	8) [ "$left" -eq "$old_left" ] && [ "$top" -eq "$old_top" ] && [ "$bottom" -eq "$old_bottom" ] || result=no ;;
	4) [ "$right" -eq "$old_right" ] && [ "$top" -eq "$old_top" ] && [ "$bottom" -eq "$old_bottom" ] || result=no ;;
	1) [ "$bottom" -eq "$old_bottom" ] && [ "$left" -eq "$old_left" ] && [ "$right" -eq "$old_right" ] || result=no ;;
	2) [ "$top" -eq "$old_top" ] && [ "$left" -eq "$old_left" ] && [ "$right" -eq "$old_right" ] || result=no ;;
	esac
	verdict $result "resize $name (edges=$edges)"
	shot after-$name
}

# Drags the title bar (its middle, empty in Files' between the path and the search) by (DX, DY); checks the new place.
move() {
	moves=$(count "KWL GLASS moved surface=$surface ")
	old_x=$x old_y=$y
	tx=$(((left + right) / 2)) ty=$((top + 22))
	pointer move $((tx - 2)) $ty sleep 200 move $tx $ty sleep 300 down sleep 200 move $((tx + $1 / 2)) $((ty + $2 / 2)) sleep 150 \
	    move $((tx + $1)) $((ty + $2)) sleep 300 up sleep 800
	expect_more "KWL GLASS moved surface=$surface " "$moves" 5 || return
	geometry
	result=ok
	[ "$x" -eq $((old_x + $1)) ] && [ "$y" -eq $((old_y + $2)) ] || result=no
	verdict $result "move $3 by $1,$2 ($old_x,$old_y -> $x,$y)"
}

# kei's session, then Files from App Home.
expect_more 'KWL HANDOFF go=1' 0 90 || { echo "C2 RESULT pass=0 fail=1"; echo "C2: FAIL"; exit 1; }
sleep 3
launches=$(count 'KWL GLASS launch(-late)? surface=')
pointer move 23 17 sleep 300 down sleep 60 up sleep 1500
set -- $(last 'KWL HOME icon name="Files"' | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\).*/\1 \2/p')
[ -n "${1:-}" ] || { echo "no Files icon"; echo "C2 RESULT pass=0 fail=1"; echo "C2: FAIL"; exit 1; }
pointer move "$1" "$2" sleep 200 down sleep 60 up
expect_more 'KWL GLASS launch(-late)? surface=' "$launches" 20 || { echo "C2 RESULT pass=$pass fail=$fail"; echo "C2: FAIL"; exit 1; }
sleep 3
surface=$(last 'KWL GLASS launch(-late)? surface=' | sed -n 's/.*surface=\([0-9]*\) .*/\1/p')
# A surface's number is its client's own: another client's window (the Welcome's Settings, ws164-p002) may have the
# same one, so Wiseview's tile is told by the client too (T1-477: the Welcome's tile was clicked).
client=$(last 'KWL GLASS launch(-late)? surface=' | sed -n 's/.* client=\([0-9]*\).*/\1/p')
width=0 height=0
geometry
echo "Files: surface $surface, ${width}x$height at $left,$top (title bar top) to $right,$bottom"
shot opened

# 1. The four corners and the four sides, each dragged outwards.
geometry; drag bottom-right 10 $((right + 3)) $((bottom + 3)) "$step" "$step" "$step" "$step"
geometry; drag top-left 5 $((left - 4)) $((top - 4)) $((-step)) $((-step)) "$step" "$step"
geometry; drag top-right 9 $((right + 3)) $((top - 4)) "$step" $((-step)) "$step" "$step"
geometry; drag bottom-left 6 $((left - 4)) $((bottom + 3)) $((-step)) "$step" "$step" "$step"
geometry; drag right 8 $((right + 3)) $(((top + bottom) / 2)) "$step" 0 "$step" 0
geometry; drag left 4 $((left - 4)) $(((top + bottom) / 2)) $((-step)) 0 "$step" 0
geometry; drag bottom 2 $(((left + right) / 2)) $((bottom + 3)) 0 "$step" 0 "$step"
geometry; drag top 1 $(((left + right) / 2)) $((top - 4)) 0 $((-step)) 0 "$step"

# 2. A move by the title bar.
geometry
move "$MOVE_DX" "$MOVE_DY" title-bar
shot moved

# 3. Maximize (a double click on the title bar) and back (a double click on the title in the system bar).
geometry
before_x=$x before_y=$y before_w=$width before_h=$height
docks=$(count "KWL GLASS dock surface=$surface ")
tx=$(((left + right) / 2)) ty=$((top + 22))
pointer move $((tx - 2)) $ty sleep 150 move $tx $ty sleep 300 down sleep 50 up sleep 120 down sleep 50 up sleep 1500
if expect_more "KWL GLASS dock surface=$surface " "$docks" 5; then
	set -- $(last "KWL GLASS dock surface=$surface " | sed -n 's/.* title=\([0-9]*\) x=\([-0-9]*\) y=\([-0-9]*\) w=\([0-9]*\) h=\([0-9]*\).*/\1 \2 \3 \4 \5/p')
	title_x=${1:-400}
	# The docked body is KWL_GLASS_DOCK_PAD in from the sides (ws099-p038): it spans the width less its x twice.
	result=ok
	[ $((${4:-0} + 2 * ${2:-0})) -eq "${size%x*}" ] || result=no
	[ "${2:-0}" -ge 0 ] && [ "${2:-0}" -le 16 ] || result=no
	verdict $result "maximize: docked ${4:-?}x${5:-?} at ${2:-?},${3:-?}"
	shot maximized
	undocks=$(count "KWL GLASS undock surface=$surface ")
	pointer move $((title_x + 20)) 17 sleep 300 down sleep 50 up sleep 120 down sleep 50 up sleep 1500
	if expect_more "KWL GLASS undock surface=$surface " "$undocks" 5; then
		set -- $(last "KWL GLASS undock surface=$surface " | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
		set -- "$1" "$2" $(last "KWL CONFIGURE client=$client surface=$surface " | sed -n 's/.* width=\([0-9]*\) height=\([0-9]*\).*/\1 \2/p')
		result=ok
		[ "$1" -eq "$before_x" ] && [ "$2" -eq "$before_y" ] && [ "${3:-0}" -eq "$before_w" ] && [ "${4:-0}" -eq "$before_h" ] || result=no
		verdict $result "unmaximize: back to $1,$2 ${3:-?}x${4:-?} (was $before_x,$before_y ${before_w}x$before_h)"
		shot restored
	fi
fi

# 4. Minimize (its button, left of maximize and close) and back from Wiseview's tile.
geometry
x=$before_x y=$before_y width=$before_w height=$before_h
left=$x right=$((x + width)) top=$((y - 52)) bottom=$((y + height))
minimizes=$(count "KWL GLASS minimize surface=$surface")
pointer move $((right - 95)) $((top + 22)) sleep 400 down sleep 60 up sleep 1200
if expect_more "KWL GLASS minimize surface=$surface" "$minimizes" 5; then
	verdict ok "minimize"
	shot minimized
	opens=$(count 'KWL WISEVIEW open windows=')
	keys '<super-tab>'
	sleep 1
	if expect_more 'KWL WISEVIEW open windows=' "$opens" 5; then
		set -- $(last "KWL WISEVIEW tile client=$client surface=$surface " | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
		selects=$(count "KWL WISEVIEW select surface=$surface client=$client")
		pointer move $((${1:-0} + ${3:-0} / 2)) $((${2:-0} + ${4:-0} / 2)) sleep 300 down sleep 60 up sleep 1500
		if expect_more "KWL WISEVIEW select surface=$surface client=$client" "$selects" 5; then
			verdict ok "unminimize from Wiseview"
			shot unminimized
			move 10 0 "after unminimize (from its place)"
		fi
	fi
fi

echo "C2 RESULT pass=$pass fail=$fail"
[ "$fail" -eq 0 ] && echo "C2: PASS" || echo "C2: FAIL"
[ "$fail" -eq 0 ]
