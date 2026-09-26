#!/bin/sh
# ws035-p069: App Home (plan/ws035/app-home-design.md) on the Venus guest.
#
# zwl --glass runs at 1280x800 with a wl_shm window.  Through QMP:
#  1. home.png: the launcher opens Home (the desktop slides to the bottom
#     right); zwl logs where the icons are.
#  2. A click on the Terminal icon starts zdesktop-terminal and closes Home;
#     the terminal's window maps (terminal.png).
#  3. gesture.png / home-drag.png: a drag from the top-left corner towards
#     the bottom right opens Home, following the pointer.
#  4. search.png: typing "mod" leaves only the Model viewer; Enter starts it
#     (mview.png).
#  5. Esc closes Home; a click on the desktop's corner closes it too.
#
#   plan/ws035/tests/zdesktop-guest.sh start     (the guest must be up)
#   plan/ws035/tests/zdesktop-p069.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p069}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='for p in $(ps -A -o pid,args | grep -E "[z]wl|[w]lshm|[w]ltest|[m]view|[z]desktop-terminal" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[z]wl|[w]lshm|[w]ltest|[m]view|[z]desktop-terminal" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
status=0

# Fails the run unless zwl's log has a line matching a pattern.
expect_log() {
	found=$(guest "grep -cE '$1' /tmp/zwl.log" | tail -1)
	if [ "${found:-0}" -gt 0 ] 2>/dev/null; then
		echo "log: $1 ok"
	else
		echo "log: $1 MISSING"
		status=1
	fi
}

# The centre of an icon, from zwl's log.
icon() {
	guest "grep 'ZWL HOME icon name=\"$1\"' /tmp/zwl.log | tail -1" | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\).*/\1 \2/p'
}

guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/zdesktop/wallpaper.ppm ] && picture=--wallpaper=/usr/share/zdesktop/wallpaper.ppm
/bin/zwl --timeout=600 --width=1280 --height=800 --glass $picture --log-frames > /tmp/zwl.log 2>&1 </dev/null & sleep 4
/bin/wlshm --size=520x340 --color=ffe8eef8 --frames=20000 --token=b > /tmp/b.log 2>&1 </dev/null & sleep 3; echo started' >/dev/null

# 1. The launcher opens Home.
pointer move 23 17 sleep 300 down sleep 60 up sleep 1200
pointer move 700 500 sleep 400
check "$out/home.png" >/dev/null
expect_log 'ZWL HOME open via=launcher'
expect_log 'ZWL HOME opened apps=4'

# 2. The Terminal icon starts the terminal and closes Home.
set -- $(icon Terminal)
echo "Terminal icon at ${1:-?},${2:-?}"
pointer move ${1:-0} ${2:-0} sleep 400 down sleep 60 up sleep 6000
pointer move 1250 780 sleep 400
check "$out/terminal.png" >/dev/null
expect_log 'ZWL HOME launch name=Terminal pid='
expect_log 'ZWL HOME close via=launch'
expect_log 'ZWL MAP client=2 '

# 3. The corner drag follows the pointer, and opens Home past the threshold.
pointer move 6 6 sleep 300 down sleep 100 \
    move 40 40 sleep 80 move 90 80 sleep 80 move 140 120 sleep 300
check "$out/gesture.png" >/dev/null
pointer move 260 220 sleep 80 move 360 320 sleep 200 up sleep 1200
pointer move 700 500 sleep 400
check "$out/home-drag.png" >/dev/null
expect_log 'ZWL HOME gesture'
expect_log 'ZWL HOME open via=drag'

# 4. Typing searches; Enter starts the one found.
keys 'mod'
sleep 1
check "$out/search.png" >/dev/null
expect_log 'ZWL HOME search query="mod" results=1 \[Model viewer\]'
keys '\n'
sleep 10
pointer move 1250 780 sleep 400
check "$out/mview.png" >/dev/null
expect_log 'ZWL HOME launch name=Model viewer pid='

# 5. Esc closes Home; so does the desktop's corner.
pointer move 23 17 sleep 300 down sleep 60 up sleep 1200
keys '<esc>'
sleep 1
expect_log 'ZWL HOME close via=escape'
pointer move 23 17 sleep 300 down sleep 60 up sleep 1200
pointer move 1268 788 sleep 500 down sleep 60 up sleep 1200
check "$out/closed.png" >/dev/null
expect_log 'ZWL HOME close via=corner'

guest 'grep -E "FAILED|ERROR" /tmp/zwl.log' | tee "$out/errors.txt"
[ -s "$out/errors.txt" ] && status=1
guest 'grep -E "ZWL (HOME|MAP)" /tmp/zwl.log' > "$out/log.txt"
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "p069: PASS" || echo "p069: FAIL"
exit $status
