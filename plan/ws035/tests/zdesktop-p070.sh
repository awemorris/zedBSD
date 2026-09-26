#!/bin/sh
# ws035-p070: X11 applications from App Home on the Venus guest (the zdesktop image, built by
# plan/ws035/tests/build-zdesktop-image.sh, run by plan/ws035/tests/zdesktop-guest.sh start).
# zdesktop --glass runs alone; App Home's "X terminal" starts zterm through /usr/libexec/zdesktop-x11, which starts
# Xzed --rootless first; "Gears" then starts zgears on the same Xzed.
#  1. xterm.png: zterm's window (a Wiseman window) after the click on its icon.
#  2. gears.png: the gears' window too; one Xzed runs.
#
#   plan/ws035/tests/zdesktop-p070.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws035-sq-run}"
out=${1:-build/ws035-p070}
mkdir -p "$out"
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
stop_all='ps -A -o pid,comm | awk "{ n = \$2; sub(\".*/\", \"\", n) } n == \"zdesktop\" || n == \"Xzed\" || n == \"zgears\" || n == \"zterm\" {print \$1}" | while read p; do kill $p; done; sleep 1'
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

# How many processes of a name run.
processes() {
	guest "ps -A -o comm | awk '{ n = \$1; sub(\".*/\", \"\", n) } n == \"$1\"' | wc -l" | tail -1 | tr -d ' '
}

# The centre of an icon, from zdesktop's log.
icon() {
	guest "grep 'ZWL HOME icon name=\"$1\"' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\).*/\1 \2/p'
}

# zdesktop alone (no X server yet).
guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0 /tmp/xzed.pid; rmdir /tmp/xzed.lock 2>/dev/null; picture=; [ -f /usr/share/zdesktop/wallpaper.ppm ] && picture=--wallpaper=/usr/share/zdesktop/wallpaper.ppm
/bin/zdesktop --timeout=600 --width=1280 --height=800 --glass $picture --log-frames > /tmp/zdesktop.log 2>&1 </dev/null & sleep 4; echo started' >/dev/null
echo "Xzed before: $(processes Xzed)"

# 1. Home, then the X terminal's icon.
pointer move 23 17 sleep 300 down sleep 60 up sleep 1500
set -- $(icon "X terminal")
echo "X terminal icon at ${1:-?},${2:-?}"
pointer move ${1:-0} ${2:-0} sleep 400 down sleep 60 up sleep 9000
pointer move 1250 780 sleep 400
check "$out/xterm.png" >/dev/null
expect_log 'ZWL HOME launch name=X terminal pid='
expect_log 'ZWL MAP client=1 '
xzed=$(processes Xzed); zterm=$(processes zterm)
echo "after X terminal: Xzed=$xzed zterm=$zterm"
[ "$xzed" = 1 ] && [ "$zterm" = 1 ] || status=1

# 2. Home again, then Gears, on the same Xzed.
pointer move 23 17 sleep 300 down sleep 60 up sleep 1500
set -- $(icon Gears)
echo "Gears icon at ${1:-?},${2:-?}"
pointer move ${1:-0} ${2:-0} sleep 400 down sleep 60 up sleep 12000
pointer move 1250 780 sleep 400
check "$out/gears.png" >/dev/null
expect_log 'ZWL HOME launch name=Gears pid='
maps=$(guest "grep -c 'ZWL MAP client=1 ' /tmp/zdesktop.log" | tail -1)
echo "Xzed's windows mapped: $maps"
[ "${maps:-0}" -ge 2 ] 2>/dev/null || status=1
xzed=$(processes Xzed); gears=$(processes zgears)
echo "after Gears: Xzed=$xzed zgears=$gears"
[ "$xzed" = 1 ] && [ "$gears" = 1 ] || status=1
guest "$stop_all" >/dev/null
[ $status -eq 0 ] && echo "zdesktop-p070: PASS (and judge the screens)" || echo "zdesktop-p070: FAIL"
exit $status
