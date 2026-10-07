#!/bin/sh
# ws074: the start page on the Venus guest (build-browser-image.sh).  zdesktop runs at 1280x800 with
# --glass and the wallpaper; browser starts without an argument at 1000x700.  Checks:
#  1. start.png: it opens /usr/share/browser/start.html (the package's start page).
#  2. about.png: a click on "About the engine" opens about.html, and Alt+Left comes back.
#  3. zdesktop's log has no ERROR line.
# The link's place comes from the host build's --dump=layout of the same page at the same width.
#
#   plan/ws074/tests/browser-guest.sh start      (the Venus guest must be up)
#   plan/ws074/tests/browser-start.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws074-run}"
export GUEST_RUNTIME
out=${1:-build/ws074-start}
mkdir -p "$out"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
pages=/usr/share/browser
stop_all='for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[b]rowser" | awk "{print \$1}"); do kill $p; done; i=0; while ps -A -o args | grep -qE "[w]ayland( |$)|[b]rowser" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
status=0

# Fails the run unless the browser's last NAVIGATE line matches.
expect_navigate() {
	last=$(guest "grep 'ZBROWSER NAVIGATE ' /tmp/b.log | tail -1")
	if printf '%s\n' "$last" | grep -qE "$1"; then
		echo "navigate: $1 ok"
	else
		echo "navigate: $1 MISSING (got: $last)"
		status=1
	fi
}

# Takes a picture with the pointer out of the way.
shot() {
	pointer move 1270 790 sleep 400
	check "$out/$1" >/dev/null
}

# The link's middle in the page at 1000 wide, from the host's layout: "x y".
f=userland/desktop/fonts
link=$(build/ws074-host/plain/browser --dump=layout --width=1000 --height=700 --font=$f/Mahora-Regular.ttf \
    --mono-font=$f/JetBrainsMono-Regular.ttf --fallback-font=$f/DroidSansFallbackFull.ttf \
    userland/desktop/browser/data/start.html |
    awk '$1 == "line" { y = $3; h = $5 } $1 == "text" && $NF == "\"About\"" { printf "%d %d\n", $2 + $4 / 2, y + h / 2; exit }')
echo "link: at $link in the page"

# zdesktop, then the browser without an argument.
guest "$stop_all" >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass $picture > /tmp/zdesktop.log 2>&1 </dev/null & sleep 4; echo started' >/dev/null
guest "export XDG_RUNTIME_DIR=/tmp; /bin/browser --width=1000 --height=700 > /tmp/b.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
set -- $(guest "grep 'KWL MAP client=' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
wx=${1:-0}; wy=${2:-0}
echo "browser: window at $wx,$wy"

# 1. The start page.
expect_navigate "path=$pages/start.html title=Browser"
shot start.png

# 2. The link to the about page, and back.
set -- $link
pointer move $((wx + $1 - 2)) $((wy + $2)) sleep 150 move $((wx + $1)) $((wy + $2)) sleep 300 down sleep 60 up sleep 1500
expect_navigate "path=$pages/about.html title=About browser"
shot about.png
pointer move $((wx + 500)) $((wy + 350)) sleep 300
keys '<alt-left>'
sleep 2
expect_navigate "path=$pages/start.html "

# 3. No error from zdesktop.
errors=$(guest "grep -c ERROR /tmp/zdesktop.log" | tail -1)
if [ "${errors:-1}" = 0 ]; then echo "zdesktop: no ERROR ok"; else echo "zdesktop: ERROR lines $errors"; status=1; fi

guest "$stop_all" >/dev/null
echo "browser-start: $([ $status = 0 ] && echo PASS || echo FAIL) ($out)"
exit $status
