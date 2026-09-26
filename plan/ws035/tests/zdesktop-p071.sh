#!/bin/sh
# ws035-p071: App Home's pages, launch animation, close drag and keys on the Venus guest (the zdesktop image).
# /etc/zdesktop/apps.conf gets 30 applications (2 pages of 24); zdesktop --glass runs with a wl_shm window.
#  1. page1.png: Home opens on page 1 of 2 (the dots at the bottom).
#  2. drag.png / page2.png: a drag to the left follows the pointer and turns to page 2.
#  3. The wheel turns back to page 1 and forward to 2; PageUp back to 1; Tab past the last icon of page 1 turns to 2.
#  4. grow.png: a click on "Second page shm" (page 2) starts it; its window grows from the icon (ZWL GLASS launch).
#  5. A drag from the bottom right towards the top left on Home closes it.
#
#   plan/ws035/tests/zdesktop-p071.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p071}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='ps -A -o pid,comm | awk "{ n = \$2; sub(\".*/\", \"\", n) } n == \"zdesktop\" || n == \"wlshm\" || n == \"Xzed\" {print \$1}" | while read p; do kill $p; done; sleep 1'
status=0

# Fails the run unless zdesktop's log has a line matching a pattern.
expect_log() {
	found=$(guest "grep -cE '$1' /tmp/zdesktop.log" | tail -1)
	if [ "${found:-0}" -gt 0 ] 2>/dev/null; then
		echo "log: $1 ok"
	else
		echo "log: $1 MISSING"
		status=1
	fi
}

# The centre of an icon, from zdesktop's log.
icon() {
	for try in 1 2 3; do
		found=$(guest "grep 'ZWL HOME icon name=\"$1\"' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([0-9]*\).*/\1 \2/p')
		[ -n "$found" ] && break
		sleep 1
	done
	echo "$found"
}

# 30 applications: Terminal, 28 fillers, and a real one on page 2.
guest "$stop_all" >/dev/null
guest 'mkdir -p /etc/zdesktop; { echo "Terminal|/bin/zdesktop-terminal|term|323a4e"; i=2; while [ $i -le 29 ]; do echo "App $i|/bin/true|filler|$(printf "%02x%02x%02x" $((i*8)) $((200-i*4)) $((80+i*5)))"; i=$((i+1)); done; echo "Second page shm|/bin/wlshm --size=420x280 --color=ff5a8de0 --frames=20000|shm page|5aa87a"; } > /etc/zdesktop/apps.conf; wc -l < /etc/zdesktop/apps.conf'
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; /bin/zdesktop --timeout=600 --width=1280 --height=800 --glass --log-frames > /tmp/zdesktop.log 2>&1 </dev/null & sleep 4
/bin/wlshm --size=520x340 --color=ffe8eef8 --frames=20000 --token=b > /tmp/b.log 2>&1 </dev/null & sleep 3; echo started' >/dev/null

# 1. Home on page 1 of 2.
pointer move 23 17 sleep 300 down sleep 60 up sleep 1200
pointer move 700 780 sleep 400
check "$out/page1.png" >/dev/null
expect_log 'ZWL HOME opened apps=30 pages=2 page=1'

# 2. A drag to the left: the pages follow, then page 2.
pointer move 900 500 sleep 200 down sleep 100 move 800 500 sleep 80 move 650 500 sleep 80 move 500 500 sleep 300
check "$out/drag.png" >/dev/null
pointer up sleep 800 move 700 780 sleep 300
check "$out/page2.png" >/dev/null
expect_log 'ZWL HOME page drag'
expect_log 'ZWL HOME page page=2 pages=2 via=drag'

# 3. The wheel, PageUp, and Tab across the page's end.
pointer wheel-up sleep 600
expect_log 'ZWL HOME page page=1 pages=2 via=wheel'
pointer wheel-down sleep 600
expect_log 'ZWL HOME page page=2 pages=2 via=wheel'
keys '<pgup>'
sleep 1
expect_log 'ZWL HOME page page=1 pages=2 via=key'
i=0
while [ $i -lt 24 ]; do keys '<tab>'; i=$((i+1)); done
sleep 1
expect_log 'ZWL HOME page page=2 pages=2 via=select'

# 4. The application on page 2 starts; its window grows from the icon.
sleep 1
set -- $(icon "Second page shm")
echo "Second page shm icon at ${1:-?},${2:-?}"
pointer move ${1:-0} ${2:-0} sleep 400 down sleep 60 up sleep 150
check "$out/grow.png" >/dev/null
sleep 3
pointer move 1250 780 sleep 400
check "$out/launched.png" >/dev/null
expect_log 'ZWL HOME launch name=Second page shm pid='
expect_log 'ZWL GLASS launch surface='

# 5. Home again, closed by a drag towards the top left.
pointer move 23 17 sleep 300 down sleep 60 up sleep 1200
pointer move 1000 650 sleep 200 down sleep 100 move 900 560 sleep 80 move 750 430 sleep 80 move 600 300 sleep 200 up sleep 1200
check "$out/closed.png" >/dev/null
expect_log 'ZWL HOME close via=drag'
guest "$stop_all; rm -f /etc/zdesktop/apps.conf" >/dev/null
[ $status -eq 0 ] && echo "zdesktop-p071: PASS (and judge the screens)" || echo "zdesktop-p071: FAIL"
exit $status
