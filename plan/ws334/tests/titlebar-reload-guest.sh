#!/bin/sh
# BUG-396: the browser's Reload as an icon in the compositor's titlebar (kl_titlebar_v1 version 6, role 17).
# One run on the Venus guest of plan/ws074/tests/build-browser-image.sh: starts the guest, zdesktop at
# 1280x800 with --glass, and the browser on its start page; takes the screen (reload.png) and a crop of
# the window's titlebar (reload-titlebar.png, three times as large); checks that the browser is still
# running (a compositor that refused role 17 would have cut it off) and stops the guest.
# The icon itself is judged on the picture.
#
#   plan/ws334/tests/titlebar-reload-guest.sh IMAGE [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
[ $# -ge 1 ] || { echo "usage: titlebar-reload-guest.sh IMAGE [OUTDIR]" >&2; exit 2; }
image=$1
out=${2:-build/tmp/titlebar-reload}
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/titlebar-reload-run}"
export GUEST_RUNTIME
mkdir -p "$out"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
status=0

timeout 600 plan/ws035/tests/zdesktop-guest.sh start "$image" || { echo "titlebar-reload: FAIL (guest)"; exit 1; }

# zdesktop, then the browser on its start page at 1000x700.
guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=600 --width=1280 --height=800 --glass $picture > /tmp/zdesktop.log 2>&1 </dev/null &
i=0; while [ ! -S /tmp/wayland-0 ] && [ $i -lt 60 ]; do sleep 1; i=$((i+1)); done; sleep 2; echo started' >/dev/null
guest 'export XDG_RUNTIME_DIR=/tmp; /bin/browser --width=1000 --height=700 /usr/share/browser/start.html > /tmp/b.log 2>&1 </dev/null & echo started' >/dev/null

# The window's place (up to 60 s), as the other browser harnesses find it.
i=0
set --
while [ $# -lt 2 ] && [ $i -lt 30 ]; do
	sleep 2
	set -- $(guest "grep 'KWL MAP client=' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
	i=$((i+1))
done
sleep 4
wx=${1:-0}; wy=${2:-0}
echo "browser: window at $wx,$wy"

# The picture, with the pointer out of the way, and the titlebar's strip over the window.
pointer move 1270 790 sleep 400
python3 plan/ws035/tests/zdesktop-check.py "$out/reload.png" --runtime "$GUEST_RUNTIME" >/dev/null
python3 - "$out/reload.png" "$out/reload-titlebar.png" "$wx" "$wy" <<'EOF'
import sys
from PIL import Image
shot = Image.open(sys.argv[1])
x, y = int(sys.argv[3]), int(sys.argv[4])
top = max(0, y - 56)
strip = shot.crop((max(0, x - 8), top, min(shot.width, x + 1008), min(shot.height, y + 8)))
strip.resize((strip.width * 3, strip.height * 3), Image.NEAREST).save(sys.argv[2])
EOF
echo "picture: $out/reload.png $out/reload-titlebar.png"

# The browser is still connected.
alive=$(guest 'ps -A -o args | grep -c "^/bin/browser"' | tail -1)
if [ "${alive:-0}" -ge 1 ]; then echo "browser: running ok"; else echo "browser: not running"; status=1; fi

timeout 120 python3 plan/tools/guest/guest.py stop >/dev/null 2>&1
sync
echo "titlebar-reload: $([ $status = 0 ] && echo PASS || echo FAIL) ($out)"
exit $status
