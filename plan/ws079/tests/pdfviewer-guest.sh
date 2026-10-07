#!/bin/sh
# ws079-p006: PDF Viewer on the Venus guest (the zdesktop image, plan/ws035/tests/zdesktop-guest.sh, with this
# worktree's pdfviewer, libpdf, files and compositor copied in by the install step).  zdesktop --glass at 1280x800.
#   install        copies the binaries, the libraries, the test documents and the demo's App Home list in
#   home           App Home shows PDF Viewer (home.png)
#   scroll         notes.pdf (3 pages) in the scroll mode: the first frame, the wheel, End (scroll-*.png)
#   page           the page mode: a swipe held in the middle, let go (page 2), Page Down (page 3) (page-*.png)
#   zoom           Ctrl+plus twice, Ctrl+0 (zoom.png)
#   chooser        Ctrl+O (chooser.png), Escape
#   annotate       Ctrl+E: Notes started on the file (Notes is copied in when this build has it), or the message
#                  that it is missing (annotate.png)
#   files          Files opens notes.pdf with PDF Viewer (files-open.png)
#   ops            the operator document's three pages in the page mode (ops-*.png)
#   resize         BUG-259: notes.pdf in a 900x600 window whose bottom right corner is dragged in by 300x180 over
#                  about a second, held still, then let go: the pages are drawn (PDFVIEWER RASTER index=0) at most 4
#                  times in all (once at the start, once when the size settles; before the fix once a step), and
#                  PDFVIEWER RESIZE settled is logged (resize-held.png while held, resize.png after)
#   real           a PDF Notes saved (/tmp/real-notes.pdf, made by hand in the annotate step's Notes) in PDF Viewer,
#                  then Annotate: Notes opens it with its strokes (real*.png)
# Every step's pictures go to OUTDIR, and to PREFIX* when a prefix is given.  The steps read the program's
# own log lines (PDFVIEWER ..., ZFILES ..., KWL ...) through SSH; nothing reads the console.
#   GUEST_RUNTIME=... plan/ws079/tests/pdfviewer-guest.sh OUTDIR PREFIX STEP...
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws079-p006-run}"
export GUEST_RUNTIME
bin=${BIN:-build/ws079-p006-amd64}
out=$1
prefix=$2
shift 2
mkdir -p "$out"
status=0
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
put() { timeout 120 python3 plan/tools/guest/guest.py put "$1" "$2" >/dev/null 2>&1 </dev/null || { echo "put $1: FAILED"; status=1; }; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@" >/dev/null; }
shot() {
	python3 plan/ws035/tests/zdesktop-check.py "$out/$1" --runtime "$GUEST_RUNTIME" >/dev/null
	[ -n "$prefix" ] && cp "$out/$1" "$prefix$1"
	echo "shot $1"
}
# Fails the run unless a log has a line matching a pattern (within a few seconds).
expect_log() {
	tries=0
	found=0
	while [ $tries -lt 10 ]; do
		found=$(guest "grep -cE '$2' $1" | tail -1)
		[ "${found:-0}" -gt 0 ] 2>/dev/null && break
		tries=$((tries + 1))
		sleep 1
	done
	if [ "${found:-0}" -gt 0 ] 2>/dev/null; then
		echo "log: $2 ok"
	else
		echo "log: $2 MISSING"
		status=1
	fi
}
stop_viewer='for p in $(ps -A -o pid,args | grep -E "[p]dfviewer|[f]iles( |$)" | awk "{print \$1}"); do kill $p; done; sleep 1'
viewer() {
	guest "$stop_viewer" >/dev/null
	guest "export XDG_RUNTIME_DIR=/tmp; /bin/pdfviewer --width=1180 --height=700 $1 > /tmp/pv.log 2>&1 </dev/null & sleep 6; echo started" >/dev/null
	expect_log /tmp/pv.log 'PDFVIEWER READY'
}

for step in "$@"; do
	case "$step" in
	install)
		put "$bin/bin/pdfviewer" /tmp/pdfviewer
		put "$bin/dynamic/libpdf.so" /tmp/libpdf.so
		put "$bin/bin/files" /tmp/files
		put "$bin/bin/wayland" /tmp/wayland
		[ -f "$bin/bin/notes" ] && put "$bin/bin/notes" /tmp/notes-program
		put build/ws079-p006-host/notes.pdf /tmp/notes.pdf
		put build/ws079-p006-host/ops.pdf /tmp/ops.pdf
		put plan/ws035/demo/apps.conf /tmp/apps.conf
		guest 'for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[p]dfviewer|[f]iles( |$)" | awk "{print \$1}"); do kill $p; done; sleep 1
cp /tmp/pdfviewer /bin/pdfviewer && cp /tmp/libpdf.so /lib/libpdf.so && cp /tmp/files /bin/files && cp /tmp/wayland /bin/wayland &&
chmod 0755 /bin/pdfviewer /bin/files /bin/wayland && chmod 0644 /lib/libpdf.so && mkdir -p /etc/keiland /tmp/pvdir && cp /tmp/apps.conf /etc/keiland/apps.conf &&
cp /tmp/notes.pdf /tmp/pvdir/notes.pdf && { [ ! -f /tmp/notes-program ] || { cp /tmp/notes-program /bin/notes && chmod 0755 /bin/notes; }; } && echo installed'
		guest 'export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=1800 --width=1280 --height=800 --glass $picture > /tmp/zdesktop.log 2>&1 </dev/null & sleep 7; echo started'
		;;
	home)
		pointer move 23 17 sleep 300 down sleep 60 up sleep 1500 move 700 780 sleep 300
		expect_log /tmp/zdesktop.log 'KWL HOME opened'
		shot home.png
		keys '<esc>'
		sleep 1
		;;
	scroll)
		viewer /tmp/notes.pdf
		expect_log /tmp/pv.log 'PDFVIEWER OPEN path=/tmp/notes.pdf pages=3'
		pointer move 640 420 sleep 200
		shot scroll-1.png
		pointer wheel-down sleep 150 wheel-down sleep 150 wheel-down sleep 150 wheel-down sleep 150 wheel-down sleep 150 wheel-down sleep 600
		shot scroll-2.png
		keys '<end>'
		sleep 1
		shot scroll-end.png
		keys '<home>'
		;;
	page)
		guest "$stop_viewer" >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp; /bin/pdfviewer --width=1180 --height=700 --mode=page /tmp/notes.pdf > /tmp/pv.log 2>&1 </dev/null & sleep 6; echo started" >/dev/null
		expect_log /tmp/pv.log 'PDFVIEWER READY'
		pointer move 640 420 sleep 300
		shot page-1.png
		pointer move 900 430 sleep 200 down sleep 80 move 850 430 sleep 40 move 780 432 sleep 40 move 700 434 sleep 40 move 620 436 sleep 40 move 540 436 sleep 500
		shot page-swipe-middle.png
		pointer move 470 436 sleep 30 up sleep 900
		expect_log /tmp/pv.log 'PDFVIEWER SWIPE .* direction=1'
		expect_log /tmp/pv.log 'PDFVIEWER PAGE shown=1'
		shot page-2.png
		keys '<pgdn>'
		sleep 1
		expect_log /tmp/pv.log 'PDFVIEWER PAGE shown=2'
		shot page-3.png
		pointer move 400 430 sleep 200 down sleep 80 move 480 430 sleep 40 move 560 430 sleep 40 move 700 430 sleep 40 move 820 430 sleep 30 up sleep 900
		expect_log /tmp/pv.log 'PDFVIEWER SWIPE .* direction=-1'
		shot page-back.png
		;;
	zoom)
		keys '<ctrl-equal>'
		sleep 1
		keys '<ctrl-equal>'
		sleep 1
		expect_log /tmp/pv.log 'PDFVIEWER (ZOOM scale=|ACTION 9 )'
		shot zoom.png
		keys '<ctrl-0>'
		sleep 1
		shot zoom-reset.png
		;;
	chooser)
		keys '<ctrl-o>'
		sleep 1
		expect_log /tmp/pv.log 'PDFVIEWER CHOOSER open'
		shot chooser.png
		keys '<esc>'
		sleep 1
		;;
	annotate)
		keys '<ctrl-e>'
		sleep 2
		expect_log /tmp/pv.log 'PDFVIEWER ANNOTATE'
		shot annotate.png
		guest 'grep ANNOTATE /tmp/pv.log'
		;;
	files)
		guest "$stop_viewer" >/dev/null
		guest 'export XDG_RUNTIME_DIR=/tmp; /bin/files --width=1000 --height=640 /tmp/pvdir > /tmp/f.log 2>&1 </dev/null & sleep 6; echo started' >/dev/null
		expect_log /tmp/f.log 'ZFILES READY'
		shot files.png
		;;
	files-open)
		expect_log /tmp/f.log 'ZFILES OPEN path=/tmp/pvdir/notes.pdf'
		guest 'grep "ZFILES OPEN" /tmp/f.log; ps -A -o pid,args | grep "[p]dfviewer"'
		sleep 4
		shot files-open.png
		;;
	ops)
		guest "$stop_viewer" >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp; /bin/pdfviewer --width=1180 --height=700 --mode=page /tmp/ops.pdf > /tmp/pv.log 2>&1 </dev/null & sleep 6; echo started" >/dev/null
		expect_log /tmp/pv.log 'PDFVIEWER READY'
		pointer move 640 420 sleep 200
		shot ops-1.png
		keys '<pgdn>'
		sleep 1
		shot ops-2.png
		keys '<pgdn>'
		sleep 1
		shot ops-3.png
		;;
	real)
		# A PDF Notes itself saved (the annotate step's Notes, a drawing and Ctrl+S; copied to /tmp/real-notes.pdf).
		guest 'for p in $(ps -A -o pid,args | grep -E "[n]otes( |$)" | awk "{print \$1}"); do kill $p; done; sleep 1' >/dev/null
		viewer /tmp/real-notes.pdf
		pointer move 640 420 sleep 200
		shot real.png
		keys '<ctrl-e>'
		sleep 4
		expect_log /tmp/pv.log 'NOTES START .* strokes=3 path=/tmp/real-notes.pdf'
		shot real-annotate.png
		;;
	resize)
		guest "$stop_viewer" >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp; /bin/pdfviewer --width=900 --height=600 /tmp/notes.pdf > /tmp/pv.log 2>&1 </dev/null & sleep 6; echo started" >/dev/null
		expect_log /tmp/pv.log 'PDFVIEWER READY'
		# The window's body from the compositor's last map, and its bottom right corner on the frame just outside it.
		line=$(guest "grep 'KWL MAP client=' /tmp/zdesktop.log | tail -1" | tail -1)
		body_x=$(printf '%s\n' "$line" | sed -n 's/.* x=\([-0-9]*\) y=.*/\1/p')
		body_y=$(printf '%s\n' "$line" | sed -n 's/.* y=\([-0-9]*\).*/\1/p')
		corner_x=$((${body_x:-0} + 900 + 3))
		corner_y=$((${body_y:-0} + 600 + 3))
		echo "resize: corner $corner_x,$corner_y"
		moves=
		index=1
		while [ $index -le 30 ]; do
			moves="$moves move $((corner_x - 10 * index)) $((corner_y - 6 * index)) sleep 30"
			index=$((index + 1))
		done
		pointer move $corner_x $corner_y sleep 300 down sleep 80 $moves sleep 700
		shot resize-held.png
		pointer up sleep 800
		expect_log /tmp/zdesktop.log 'KWL RESIZE start'
		expect_log /tmp/pv.log 'PDFVIEWER RESIZE settled'
		rasters=$(guest "grep -c 'PDFVIEWER RASTER index=0 ' /tmp/pv.log" | tail -1)
		if [ "${rasters:-99}" -le 4 ] 2>/dev/null; then
			echo "resize: page 1 drawn $rasters times ok"
		else
			echo "resize: page 1 drawn ${rasters:-?} times FAILED (at most 4)"
			status=1
		fi
		shot resize.png
		;;
	stop)
		guest "$stop_viewer" >/dev/null
		;;
	*)
		echo "unknown step $step"
		status=1
		;;
	esac
done
guest 'grep -c ERROR /tmp/zdesktop.log' | tail -1 | sed 's/^/zdesktop ERROR lines: /'
exit $status
