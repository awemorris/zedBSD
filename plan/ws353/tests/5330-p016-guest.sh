#!/bin/sh
# The compositor's cursor plane, input and drawing on the Latitude 5330's own display (zedBSD, i915), run as root on
# zedBSD by plan/ws353/tests/5330-p016.sh.  The graphical login (the greeter service, sessiond) is stopped and a compositor of its own (--testing --glass
# --log-frames) shows on the panel; the test kernel's injector (CONFIG_INPUT_TEST_INJECT, aat-input) moves the
# pointer and types.  Everything goes to DIR (default /tmp/p016): kwl.log (the compositor's log), marks.txt (the
# log's line count at each step's start and end), the clients' output, and results.txt (one line per step, counted
# from kwl.log here).  Nothing is judged from a console.
#   sh 5330-p016-guest.sh [DIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
D=${1:-/tmp/p016}
mkdir -p "$D/home"
L=$D/kwl.log
R=$D/results.txt
: > "$R"
: > "$D/marks.txt"
export XDG_RUNTIME_DIR=$D HOME=$D/home
lines() {
	wc -l < "$L" | tr -d ' '
}
mark() {
	echo "$1 $(lines)" >> "$D/marks.txt"
}
# The log's lines between two marks (by name).
between() {
	a=$(awk -v m="$1" '$1 == m { print $2 }' "$D/marks.txt" | tail -1)
	b=$(awk -v m="$2" '$1 == m { print $2 }' "$D/marks.txt" | tail -1)
	awk -v a="$a" -v b="$b" 'NR > a && NR <= b' "$L"
}
say() {
	echo "$*" >> "$R"
}
wait_for() {
	n=0
	while ! grep -q "$1" "$L" 2> /dev/null; do
		n=$((n + 1))
		[ "$n" -gt "$2" ] && return 1
		sleep 1
	done
	return 0
}
aat() {
	timeout 10 aat-input "$@" >> "$D/aat.log" 2>&1
}
# Ends every process of a name (there is no pkill).
killname() {
	for p in $(ps -A -o pid,comm | awk -v n="$1" '$2 == n { print $1 }'); do
		kill "$p" 2> /dev/null
	done
}
client() {
	WAYLAND_DISPLAY=/tmp/wayland-0 timeout "$1" $2 > "$D/$3.txt" 2>&1 < /dev/null &
}

service stop greeter > "$D/stop.txt" 2>&1
sleep 5
killname sessiond
sleep 2
killname wayland
sleep 2
ps -A -o pid,comm,args > "$D/ps-before.txt" 2>&1
/bin/wayland --testing --glass --log-frames --rotate-policy=any --timeout=1500 \
    --wallpaper=/usr/share/keiland/wallpaper.png > "$L" 2>&1 < /dev/null &
cpid=$!
if wait_for 'KWL MODE window' 90; then
	say "start: $(grep -m1 'KWL READY' "$L")"
else
	say "start: FAIL no KWL MODE window in 90 s"
	exit 1
fi
sleep 3
w=$(sed -n 's/.*KWL READY .*width=\([0-9]*\) height=\([0-9]*\).*/\1/p' "$L" | head -1)
h=$(sed -n 's/.*KWL READY .*width=\([0-9]*\) height=\([0-9]*\).*/\2/p' "$L" | head -1)
[ "${w:-0}" -gt 400 ] && [ "${h:-0}" -gt 400 ] || { w=1920; h=1080; }
say "1 open: open=$(grep -c 'KWL CURSOR_PLANE open' "$L") unavailable=$(grep -c 'KWL CURSOR_PLANE unavailable' "$L") $(grep -m1 'KWL CURSOR_PLANE open' "$L")"

# The injector's devices come after the start (the hotplug thread opens them).
mark aat
aat-input start --width "$w" --height "$h" > "$D/aat.log" 2>&1
sleep 3
mark aat-end
say "devices: $(between aat aat-end | grep -c 'KWL INPUT\|KWL DEVICES') input lines after the injector's start"

# 2. Pointer moves for about 4 s, as fast as the injector takes them.
mark move
i=0
t0=$(date +%s)
while [ "$i" -lt 400 ]; do
	x=$((100 + (i * 7) % (w - 200)))
	y=$((100 + (i * 3) % (h - 200)))
	aat move-to "$x" "$y"
	i=$((i + 1))
done
t1=$(date +%s)
sleep 1
mark move-end
between move move-end > "$D/move.log"
say "2 move: moves=400 in $((t1 - t0)) s, present copy=0 $(grep -c 'KWL CURSOR_PLANE present .*copy=0' "$D/move.log"), copy=1 $(grep -c 'KWL CURSOR_PLANE present .*copy=1' "$D/move.log"), failed $(grep -c 'KWL CURSOR_PLANE failed' "$D/move.log")"
say "2 move perf: $(grep 'KWL PERF cursor' "$D/move.log" | tail -2 | tr '\n' ';')"
say "2 move xy: first $(grep -m1 'KWL CURSOR_PLANE present' "$D/move.log" | cut -c1-80) last $(grep 'KWL CURSOR_PLANE present' "$D/move.log" | tail -1 | cut -c1-80)"

# 8. 60 s with no input: the compositor's timeouts and the cursor thread's sleeps.
mark idle
sleep 65
mark idle-end
between idle idle-end > "$D/idle.log"
say "8 idle: present lines $(grep -c 'KWL CURSOR_PLANE present' "$D/idle.log"), PERF windows: $(grep 'KWL PERF [0-9]*ms' "$D/idle.log" | sed 's/ |.*//' | tr '\n' ';')"
say "8 idle timers: $(grep 'KWL PERF timers' "$D/idle.log" | tr '\n' ';')"

# 3. Cursor images: a window's edges (the compositor's resize cursors) and a text client (its I-beam).
mark image
client 120 "wlshm --size=600x400 --band=ff00ff00 --frames=100000 --token=edge" edge
sleep 4
win=$(grep 'KWL GLASS launch\|KWL GLASS place\|KWL CONFIGURE' "$L" | tail -3 | tr '\n' ';')
cx=$((w / 2))
cy=$((h / 2))
for d in 0 1 2 3 4 5; do
	aat move-to "$cx" "$cy"
	sleep 0.3
	aat move-to $((cx - 300 - d)) "$cy"
	sleep 0.3
	aat move-to $((cx + 300 + d)) $((cy + 200 + d))
	sleep 0.3
	aat move-to 5 5
	sleep 0.3
done
client 60 "textedit" text
sleep 5
for d in 0 1 2 3 4 5 6 7 8 9; do
	aat move-to $((cx - 200 + d * 40)) $((cy - 100 + d * 20))
	sleep 0.3
done
sleep 1
mark image-end
between image image-end > "$D/image.log"
say "3 image: copy=1 $(grep -c 'KWL CURSOR_PLANE present .*copy=1' "$D/image.log"), copy=0 $(grep -c 'KWL CURSOR_PLANE present .*copy=0' "$D/image.log"), failed $(grep -c 'KWL CURSOR_PLANE failed' "$D/image.log"); windows $win"

# Key repeat: a key held 2 s in the text client.
mark key
aat key-down a
sleep 2
aat key-up a
sleep 1
mark key-end
between key key-end > "$D/key.log"
say "key: KWL key lines $(grep -c 'KWL KEY\|KWL REPEAT\|repeat' "$D/key.log"), text client said $(tail -c 200 "$D/text.txt" | tr '\n' ' ')"

# 4. A drag: the left button held across the screen (a drag's icon only when a client starts one).
mark drag
aat drag $((cx - 100)) $((cy - 100)) $((cx + 200)) $((cy + 150)) 40
sleep 1
mark drag-end
between drag drag-end > "$D/drag.log"
say "4 drag: copy=1 $(grep -c 'KWL CURSOR_PLANE present .*copy=1' "$D/drag.log"), copy=0 $(grep -c 'KWL CURSOR_PLANE present .*copy=0' "$D/drag.log"), dnd lines $(grep -c 'KWL DND\|KWL DRAG' "$D/drag.log")"
killname textedit
killname wlshm
sleep 2

# Drawing speed: wlshm on frame callbacks and wltest FIFO, and the present's acceptance (KWL LAT submit to shown).
mark speed
t0=$(date +%s)
WAYLAND_DISPLAY=/tmp/wayland-0 timeout 90 wlshm --size=400x300 --band=ff00ff00 --frames=600 --token=shm > "$D/shm.txt" 2>&1 < /dev/null
t1=$(date +%s)
WAYLAND_DISPLAY=/tmp/wayland-0 timeout 90 wltest --display=/tmp/wayland-0 --mode=fifo --frames=300 --token=vk > "$D/vk.txt" 2>&1 < /dev/null
t2=$(date +%s)
mark speed-end
between speed speed-end > "$D/speed.log"
say "speed: wlshm 600 frames $((t1 - t0)) s rc tail: $(tail -1 "$D/shm.txt"); wltest 300 fifo $((t2 - t1)) s tail: $(tail -1 "$D/vk.txt")"
say "speed lat: $(awk '$2 == "LAT" && $3 == "submit" { split($4, f, "="); split($5, t, "="); s[f[2]] = t[2] } $2 == "LAT" && $3 == "shown" { split($4, f, "="); split($6, t, "="); if (f[2] in s) { d = t[2] - s[f[2]]; n++; sum += d; if (d > mx) mx = d } } END { if (n) printf "submit->shown n=%d mean_us=%.0f max_us=%d", n, sum / n, mx; else print "no submit/shown pairs" }' "$D/speed.log")"
say "speed returns: $(grep -c 'KWL RETURN' "$D/speed.log") KWL RETURN lines; read>done violations $(awk '$2 == "RETURN" { split($5, r, "="); split($6, d, "="); if (r[2] + 0 > d[2] + 0) v++ } END { print v + 0 }' "$D/speed.log")"

# 5. The game mode: mview fullscreen, then the pointer again.
mark game
WAYLAND_DISPLAY=/tmp/wayland-0 timeout 40 mview --token=spin --spin=5 --copies=1 --content=game --present=mailbox > "$D/mview.txt" 2>&1 < /dev/null
sleep 2
mark game-out
i=0
while [ "$i" -lt 60 ]; do
	aat move-to $((200 + i * 10)) $((200 + i * 5))
	i=$((i + 1))
done
sleep 1
mark game-end
between game game-out > "$D/game.log"
between game-out game-end > "$D/game-after.log"
say "5 game: $(grep 'MVIEW DONE' "$D/mview.txt"); scanout $(grep 'KWL SCANOUT' "$D/game.log" | cut -c1-60 | tr '\n' ';') park failures $(grep -c 'KWL FAILED site=park' "$D/game.log"); after: present $(grep -c 'KWL CURSOR_PLANE present' "$D/game-after.log")"

# 7. The end: SIGTERM, the cursor thread's and the shown thread's counts.
aat stop
mark stop
kill "$cpid"
n=0
while kill -0 "$cpid" 2> /dev/null && [ "$n" -lt 20 ]; do
	sleep 1
	n=$((n + 1))
done
mark stop-end
say "7 stop: $(grep 'KWL CURSOR_PLANE stopped' "$L" | tail -1)"
say "7 shown: $(grep 'KWL SHOWN stopped' "$L" | tail -1)"
say "failed: CURSOR_PLANE failed $(grep -c 'KWL CURSOR_PLANE failed' "$L"), KWL FAILED $(grep -c 'KWL FAILED' "$L"), VULKAN_ERROR $(grep -c 'VULKAN_ERROR' "$L")"
say "done"
service start greeter > "$D/start.txt" 2>&1
