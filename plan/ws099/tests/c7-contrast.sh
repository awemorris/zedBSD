#!/bin/sh
# ws099-p001, C7: the text on the frosted glass keeps a contrast of C7_MIN_CONTRAST (WCAG AA 4.5:1) on the default
# wallpaper and the 5 generated ones (/usr/share/keiland/wallpapers).  For each, zdesktop --glass at 1280x800 is
# started with it and a popup-probe window (centred, 400x300 at 440,293); the screenshot's system bar clock and the
# window's title are measured (plan/ws099/tests/c7-contrast.py: median glass against the darkest 1%).
# ws099-p005: then the text the applications draw on their glass panels: Settings' Home page (its title, subtitle,
# a section heading, two sidebar rows, the title bar) and Files' Home (a heading, a sidebar group label, a sidebar row,
# the title bar).  These count against C7_MIN_CONTRAST; the text of inactive items (Files' sidebar rows of folders
# that do not exist) and the empty list's hint are measured and reported apart ("info"), as WCAG exempts inactive
# components (the hint is reported for the user to judge).
#
#   plan/ws035/tests/zdesktop-guest.sh start build/ws099-criteria.img
#   plan/ws099/tests/c7-contrast.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws035-sq-run}"
export GUEST_RUNTIME
out=${1:-build/ws099-shots/c7}
C7_MIN_CONTRAST=${C7_MIN_CONTRAST:-4.5}
# The regions (x0 y0 x1 y1) of the text measured at 1280x800: the system bar's clock and the probe's title.
C7_CLOCK=${C7_CLOCK:-1130 4 1272 30}
C7_TITLE=${C7_TITLE:-480 250 600 278}
# The applications' text (x0 y0 x1 y1 at 1280x800, the windows' default places), NAME:REGION pairs.
# 2026-10-05 (T1-169, ws138-p002): s-section, f-group, f-side-recents, f-hint and f-inactive (now the Desktop row) moved to where the labels are now (Files'
# sidebar gained Today and Home in ws127-p011; Settings' section heading sits lower); measured on the T1-169 shots.
# 2026-10-10 (T1-504): s-section up 12 to the heading's place under libkeiland's kl_header (ws090-p023, 2026-10-06; the
# old box held no text, text=glass on every wallpaper); measured on the T1-504 shots (ink at 340..418 x 207..217).
C7_SETTINGS=${C7_SETTINGS:-"s-title 338 130 466 160 s-subtitle 338 166 592 186 s-section 340 204 436 222 s-side-wifi 100 115 143 133 s-side-appearance 100 309 194 327 s-bar-title 94 59 155 77"}
C7_FILES=${C7_FILES:-"f-heading 326 324 394 341 f-group 96 404 160 416 f-side-recents 124 432 182 446 f-bar-home 290 59 335 77"}
C7_INFO=${C7_INFO:-"f-hint 326 426 507 443 f-inactive 124 214 184 230"}
C7_CLIENTS=${C7_CLIENTS:-1}
mkdir -p "$out"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
# The processes are told by their command (argv[0], ps -o comm), not by their command line: since BUG-274 ps -o args
# shows whole lines, and the guest shell running "$ends; ... /bin/settings ..." names settings in its own, so a match
# on the line killed that shell before it started Settings (T1-477: no Settings or Files window on any wallpaper).
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,comm | awk "\$2 ~ /(^|\\/)(wayland|popup-probe|settings|files)\$/ {print \$1}"); do kill $p; done; i=0; while ps -A -o comm | grep -qE "(^|/)(wayland|popup-probe|settings|files)$" && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done'
ends='for p in $(ps -A -o pid,comm | awk "\$2 ~ /(^|\\/)(popup-probe|settings|files)\$/ {print \$1}"); do kill $p; done; sleep 1; echo ok'
pass=0
fail=0
worst=

pictures=$(guest 'ls /usr/share/keiland/wallpaper.png /usr/share/keiland/wallpapers/*.png 2>/dev/null' | grep '\.png$')
echo "wallpapers: $(echo $pictures | wc -w)"
for picture in $pictures; do
	name=$(basename "$picture" .png)
	guest "$stop_all" >/dev/null
	guest "export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0
/bin/wayland --testing --timeout=300 --width=1280 --height=800 --glass --wallpaper=$picture > /tmp/zdesktop.log 2>&1 </dev/null & i=0; while [ ! -S /tmp/wayland-0 ] && [ \$i -lt 60 ]; do sleep 0.5; i=\$((i+1)); done; sleep 1
/bin/popup-probe --timeout-s=200 --token=c > /tmp/c.log 2>&1 </dev/null & sleep 3; echo started" >/dev/null
	pointer move 5 790 sleep 600
	check "$out/$name.png" >/dev/null
	lines=$(python3 plan/ws099/tests/c7-contrast.py "$out/$name.png" clock $C7_CLOCK title $C7_TITLE)
	echo "$lines" | sed "s/^C7 /C7 $name /"
	# The applications' glass: Settings, then Files (HOME=/root).
	if [ "$C7_CLIENTS" = 1 ]; then
		guest "$ends; export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/settings --timeout-s=200 > /tmp/s.log 2>&1 </dev/null & sleep 6; echo ok" >/dev/null
		pointer move 5 790 sleep 500
		check "$out/$name-settings.png" >/dev/null
		more=$(python3 plan/ws099/tests/c7-contrast.py "$out/$name-settings.png" $C7_SETTINGS)
		echo "$more" | sed "s/^C7 /C7 $name /"
		lines="$lines
$more"
		guest "$ends; export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/files --timeout-s=200 > /tmp/f.log 2>&1 </dev/null & sleep 8; echo ok" >/dev/null
		pointer move 5 790 sleep 500
		check "$out/$name-files.png" >/dev/null
		more=$(python3 plan/ws099/tests/c7-contrast.py "$out/$name-files.png" $C7_FILES)
		echo "$more" | sed "s/^C7 /C7 $name /"
		lines="$lines
$more"
		python3 plan/ws099/tests/c7-contrast.py "$out/$name-files.png" $C7_INFO | sed "s/^C7 /C7 info $name /"
	fi
	for ratio in $(echo "$lines" | sed -n 's/.*contrast=\([0-9.]*\).*/\1/p'); do
		if python3 -c "import sys; sys.exit(0 if $ratio >= $C7_MIN_CONTRAST else 1)"; then
			pass=$((pass + 1))
		else
			fail=$((fail + 1))
		fi
		worst=$(python3 -c "print(min([float(v) for v in '$worst $ratio'.split()]))")
	done
done
guest "$stop_all" >/dev/null
echo "C7 RESULT pass=$pass fail=$fail min_contrast=$worst limit=$C7_MIN_CONTRAST"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ] && echo "C7: PASS" || echo "C7: FAIL"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
