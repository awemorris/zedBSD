#!/bin/sh
# ws094-p003: Files' desktop mode (files --desktop) on zdesktop's desktop surface, on the Venus guest.  The running
# guest gets this worktree's compositor, files and libraries (BIN); HOME is /tmp/dhome with a Desktop folder of a
# folder, a text, a picture, a PDF and a script.  zdesktop --glass at 1280x800 starts /bin/files --desktop itself
# (--desktop-client), with the token in its environment.
#   show      the desktop: the role taken, configured 1280x756, the five items laid out from the top-right corner down
#             (ZFILES DESKTOP place/ready), a picture (desktop.png)
#   watch     a file added to ~/Desktop appears within a few seconds (items=6, added.png), and goes when it is removed
#   input     click, arrow, Enter (the started program has no token), a double click on a folder (a new window), a
#             rubber band (selected.png, folder.png, band.png) and its frames' times (BUG-221)
#   window    a Files window opens over the icons (window.png)
#   saved     (ws094-p004) the layout file placed before the desktop starts puts notes.txt at column 2 row 3 (its saved
#             place), the other items in the free cells (saved.png)
#   prune     (ws094-p011) a stale saved name remains on disk until a drag saves the layout, then disappears;
#             overflow is counted without showing extra icons (prune.png, overflow.png)
#   menu      (ws094-p005, after show) the context menus chosen with the pointer: the empty desktop's New Folder named
#             Plans in its field (the new item in the free cell, kept there under the new name); notes.txt renamed to
#             todo.txt from its menu (it keeps its cell); photo.png copied and pasted on the empty desktop (the name is
#             taken: the question over the desktop, Enter keeps both); script.sh moved to the trash with Delete; Show in
#             Files on report.pdf (a Files window on ~/Desktop; the desktop's menu stays open beside it, and closes at a
#             press on the window and when a new window maps); Clean Up (the items in order again); Change Wallpaper
#             when Settings is installed (menu-empty.png, rename-field.png, menu-item.png, collision.png, trash.png,
#             show-in-files.png, cleanup.png)
#   drag      (ws094-p006; starts the desktop again from the five items) the pointer drags: notes.txt to an empty cell (moved there and saved; drag-over.png,
#             drag-moved.png); photo.png onto the folder Projects (moved into it); a Files window's note.txt out to the
#             desktop (moved into ~/Desktop, placed at the cell of the drop; drop-in.png); the desktop's report.pdf onto the
#             Files window (moved into its folder; drop-out.png)
#   perf100   (ws094-p008, L3) the desktop with 100 items: start to ready, a file added to shown, a click to the selection's
#             frame, three rounds (perf100.txt: the rounds, the medians against the targets; perf100.png); ws094-p009: start to
#             zdesktop's first drawing of the desktop, and the steps of Files' start (perf100-steps.txt)
#   touch     (ws094-p006, after show; the pen image, /bin/touchinject) a double tap on Projects opens it, a long press on
#             report.pdf opens its context menu (touch-menu.png), a long press that moves drags script.sh to an empty cell
#             (touch-moved.png)
# The steps read zdesktop's log (Files, started by zdesktop, writes there too) through SSH, and the pictures; nothing
# reads the console.
#   GUEST_RUNTIME=$PWD/build/ws094-run BIN=build/ws094-amd64 plan/ws094/tests/files-desktop-guest.sh OUTDIR STEP...
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws094-run}"
bin=${BIN:-build/ws094-amd64}
out=$1
shift
mkdir -p "$out"
status=0
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
put() { timeout 120 python3 plan/tools/guest/guest.py put "$1" "$2" >/dev/null 2>&1 </dev/null || { echo "put $1: FAILED"; status=1; }; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@" >/dev/null; sleep 0.7; }
shot() {
	python3 plan/ws035/tests/zdesktop-check.py "$out/$1" --runtime "$GUEST_RUNTIME" >/dev/null
	echo "shot $1"
}
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[f]iles" | awk "{print \$1}"); do kill $p; done; sleep 1'

# The middle (y) of the latest popup row of an item, and the latest popup's left edge (depth 1 or 2), from zdesktop's log.
row_y() {
	guest "grep -a 'MENU row item=$1 ' /tmp/zdesktop.log | tail -1" | sed -n 's/.* y=\([0-9]*\) height=\([0-9]*\).*/\1 \2/p' | { read y h; echo $(( ${y:-0} + ${h:-0} / 2 )); }
}
popup_x() {
	guest "grep -a 'MENU open .* depth=${1:-1} ' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([0-9]*\) y=.*/\1/p'
}

# Replays a touch script (the pen image's injected touch screen), screen pixels of 1280x800.
touch_replay() {
	printf '%s\n' "$2" > "$out/$1.script"
	put "$out/$1.script" "/tmp/$1.script"
	result=$(guest "/bin/touchinject /tmp/$1.script 2>&1; echo replay=\$?")
	printf '%s\n' "$result" | grep -q '^replay=0$' || { echo "touchinject $1: FAILED"; status=1; }
}

# Clicks and right-clicks a screen point; choose clicks a row of the open context menu (by its item number).
click() { pointer move $(($1 - 2)) "$2" sleep 150 move "$1" "$2" sleep 300 down sleep 60 up sleep "${3:-900}"; }
rclick() { pointer move $(($1 - 2)) "$2" sleep 150 move "$1" "$2" sleep 300 right-down sleep 60 right-up sleep 1200; }
choose() {
	x=$(popup_x 1)
	click $(( ${x:-0} + 60 )) "$(row_y "$1")" 1500
}

# Fails the run unless a log has a line matching a pattern (within a few seconds).
expect_log() {
	tries=0
	found=0
	while [ $tries -lt 10 ]; do
		found=$(guest "grep -acE '$2' $1" | tail -1)
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

# Makes ~/Desktop the five items again (a folder, a text, a picture, a PDF and a script), as install leaves it.  saved and
# prune start from it, since menu and drag rename, move and delete items (ws099-p024: saved after menu found no notes.txt).
fresh_desktop() {
	guest 'rm -rf /tmp/dhome/Desktop; mkdir -p /tmp/dhome/Desktop/Projects; printf "Meeting notes\n" > /tmp/dhome/Desktop/notes.txt; printf "#!/bin/sh\necho hi\n" > /tmp/dhome/Desktop/script.sh; chmod 755 /tmp/dhome/Desktop/script.sh' >/dev/null
	put build/ws094-images/01-splash.png /tmp/dhome/Desktop/photo.png
	put build/ws094-images/report.pdf /tmp/dhome/Desktop/report.pdf
}

for step in "$@"; do
	case "$step" in
	install)
		# The greeter's compositor of a graphical image holds /bin/wayland open (ws094-p008); just after the boot it may
		# still be ending, so the two programs are tried a few times (ws094-p013).
		guest "$stop_all" >/dev/null
		python3 plan/tools/imageview/make-images.py build/ws094-images >/dev/null
		for program in wayland files; do
			tries=0
			until timeout 120 python3 plan/tools/guest/guest.py put "$bin/bin/$program" "/bin/$program" >/dev/null 2>&1 </dev/null; do
				tries=$((tries + 1))
				[ $tries -lt 3 ] || { echo "put $bin/bin/$program: FAILED"; status=1; break; }
				guest "$stop_all" >/dev/null
				sleep 3
			done
		done
		for library in $(cd "$bin/dynamic" && ls *.so | grep -vE '^(libc|ld)\.so$'); do
			put "$bin/dynamic/$library" "/lib/$library"
		done
		guest 'chmod 755 /bin/wayland /bin/files; rm -rf /tmp/dhome' >/dev/null
		python3 plan/ws081/tests/make-touch-pdf.py build/ws094-images/report.pdf >/dev/null
		fresh_desktop
		;;
	show)
		guest "$stop_all" >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/tmp/dhome; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass \$picture --desktop-client='/bin/files --desktop' > /tmp/zdesktop.log 2>&1 </dev/null &
i=0; while ! grep -aq 'ZFILES READY' /tmp/zdesktop.log && [ \$i -lt 60 ]; do sleep 0.5; i=\$((i+1)); done; sleep 2; echo started" >/dev/null
		expect_log /tmp/zdesktop.log 'KWL DESKTOP start pid=[0-9]+ command=/bin/files --desktop'
		expect_log /tmp/zdesktop.log 'KWL DESKTOP role client=[0-9]+ surface=[0-9]+ x=0 y=44 width=1280 height=756'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP configure x=0 y=44 width=1280 height=756'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP ready items=5 cells=5 width=1280 height=756'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=[^ ]+ column=0 row=0 x=1168 y=16'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=[^ ]+ column=0 row=4 x=1168 y=432'
		pointer move 640 600 sleep 300
		sleep 2
		shot desktop.png
		guest "grep -a 'ZFILES DESKTOP place' /tmp/zdesktop.log" > "$out/places.txt"
		;;
	watch)
		guest 'printf "added\n" > /tmp/dhome/Desktop/added.txt' >/dev/null
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP ready items=6 cells=6'
		sleep 1
		shot added.png
		guest 'rm -f /tmp/dhome/Desktop/added.txt' >/dev/null
		sleep 3
		found=$(guest "grep -ac 'ZFILES DESKTOP ready items=5' /tmp/zdesktop.log" | tail -1)
		[ "${found:-0}" -ge 2 ] 2>/dev/null && echo "removed: ok" || { echo "removed: MISSING"; status=1; }
		;;
	input)
		# Icons are placed from the top-right corner: 1 Projects (1216,90), 2 notes.txt (1216,194), 3 photo.png (1216,298).
		# ~/.config/keiland/open-with sends plain text to "Env" (env > ~/env.txt): the program a double click starts
		# must not have the desktop's token.
		guest 'mkdir -p /tmp/dhome/.config/keiland; printf "text/plain\tEnv\tenv > /tmp/dhome/env.txt # %%f\n" > /tmp/dhome/.config/keiland/open-with; rm -f /tmp/dhome/env.txt' >/dev/null
		pointer move 1216 298 sleep 300 down sleep 60 up sleep 700
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP select name=photo.png selected=1'
		shot selected.png
		keys '<up>'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP select name=notes.txt selected=1 via=arrow'
		keys '<ret>'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP open via=enter'
		expect_log /tmp/zdesktop.log 'ZFILES OPEN path=/tmp/dhome/Desktop/notes.txt app=Env error=0'
		sleep 2
		token=$(guest 'grep -c KEILAND_DESKTOP_TOKEN /tmp/dhome/env.txt; grep -c "^HOME=" /tmp/dhome/env.txt' | tail -2 | tr '\n' ' ')
		[ "$token" = "0 1 " ] && echo "token: not inherited ok" || { echo "token: ($token) MISSING"; status=1; }
		# A double click on the folder opens a new Files window on it.
		pointer move 1216 90 sleep 300 down sleep 50 up sleep 80 down sleep 50 up sleep 3000
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP open name=Projects via=double-click'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP open-folder path=/tmp/dhome/Desktop/Projects error=0'
		expect_log /tmp/zdesktop.log 'KWL MAP client=[0-9]+ '
		shot folder.png
		# The folder's window (on top, with the keyboard) closes by Ctrl+W.
		keys '<ctrl-w>'
		sleep 1
		# A rubber band from empty desktop over the first column selects its items.
		# The picture is taken after one more small move and a rest, so that the band's last frame is shown (the
		# first p004 picture was one frame behind the pointer).
		pointer move 1100 60 sleep 300 down sleep 100 move 1150 200 sleep 100 move 1260 330 sleep 400 move 1261 331 sleep 1200
		shot band.png
		pointer up sleep 500
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP band start x=1100 y=16'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP band end first=0'
		# BUG-221: the band's frames and their times (the band's frames draw only where it changed).
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP band frames=[0-9]+ mean_ms=[0-9]+ longest_ms=[0-9]+ draw_ms=[0-9]+ present_ms=[0-9]+'
		guest "grep 'DESKTOP band frames=' /tmp/zdesktop.log | tail -1"
		keys '<esc>'
		;;
	window)
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/tmp/dhome; /bin/files --token=w --timeout-s=800 --width=800 --height=560 /tmp/dhome/Desktop > /tmp/f.log 2>&1 </dev/null & sleep 6; echo started" >/dev/null
		expect_log /tmp/zdesktop.log 'KWL MAP client=[0-9]+ '
		pointer move 640 760 sleep 300
		shot window.png
		;;
	saved)
		# notes.txt kept at column 2, row 3 before Files starts; the compositor started again with Files.
		guest "$stop_all" >/dev/null
		fresh_desktop
		guest 'mkdir -p /tmp/dhome/.config/keiland; printf "notes.txt\t2\t3\n" > /tmp/dhome/.config/keiland/desktop-layout' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/tmp/dhome; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass \$picture --desktop-client='/bin/files --desktop' > /tmp/zdesktop.log 2>&1 </dev/null &
i=0; while ! grep -aq 'ZFILES READY' /tmp/zdesktop.log && [ \$i -lt 60 ]; do sleep 0.5; i=\$((i+1)); done; sleep 2; echo started" >/dev/null
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=notes.txt column=2 row=3 '
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=Projects column=0 row=0 '
		pointer move 640 600 sleep 300
		sleep 2
		shot saved.png
		guest "grep -a 'ZFILES DESKTOP place' /tmp/zdesktop.log" > "$out/saved-places.txt"
		guest 'rm -f /tmp/dhome/.config/keiland/desktop-layout' >/dev/null
		;;
	prune)
		# Keep notes.txt at its saved cell, and prune only the name absent from the successful listing.
		guest "$stop_all" >/dev/null
		fresh_desktop
		guest 'mkdir -p /tmp/dhome/.config/keiland; printf "notes.txt\t2\t3\nghost.txt\t4\t4\n" > /tmp/dhome/.config/keiland/desktop-layout' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/tmp/dhome; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass \$picture --desktop-client='/bin/files --desktop' > /tmp/zdesktop.log 2>&1 </dev/null &
i=0; while ! grep -aq 'ZFILES READY' /tmp/zdesktop.log && [ \$i -lt 60 ]; do sleep 0.5; i=\$((i+1)); done; sleep 2; echo started" >/dev/null
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP prune removed=1 kept=1'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=notes.txt column=2 row=3 '
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP ready items=5 cells=5 .* hidden=0$'
		expect_log /tmp/dhome/.config/keiland/desktop-layout '^ghost.txt'
		# Drag notes.txt from its saved cell 2,3 to 3,3, using the ordinary save path.
		pointer move 1024 402 sleep 300 down sleep 150 move 1015 402 sleep 100 move 980 400 sleep 100 \
			move 950 400 sleep 100 move 900 400 sleep 900 up sleep 1500
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP move name=notes.txt column=3 row=3 error=0'
		layout=$(guest 'cat /tmp/dhome/.config/keiland/desktop-layout')
		printf '%s\n' "$layout" > "$out/pruned-layout.txt"
		printf '%s\n' "$layout" | grep -q '^ghost.txt' && { echo 'prune: stale saved name remains'; status=1; }
		expected=$(printf 'notes.txt\t3\t3')
		printf '%s\n' "$layout" | grep -qx "$expected" || { echo 'prune: moved saved name MISSING'; status=1; }
		shot prune.png
		# Exactly 100 items exceed the 13-by-6 grid (756 pixels under the 44-pixel bar, ws099-p031) by 22, with their display
		# policy preserved.
		guest 'i=0; while [ $i -lt 95 ]; do printf x > /tmp/dhome/Desktop/overflow-$i.txt; i=$((i+1)); done' >/dev/null
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP ready items=100 cells=78 .* hidden=22$'
		shot overflow.png
		guest "grep -aE 'DESKTOP (prune|ready)' /tmp/zdesktop.log" > "$out/prune-log.txt"
		# The overflow's files go again, so that the steps after this one find the desktop as it was (T1-257: the menu
		# step's empty place at 700,400 was an icon of the overflow).
		before=$(guest "grep -ac 'ZFILES DESKTOP ready items=5 ' /tmp/zdesktop.log" | tail -1)
		guest 'rm -f /tmp/dhome/Desktop/overflow-*.txt' >/dev/null
		tries=0
		now=0
		while [ $tries -lt 10 ]; do
			now=$(guest "grep -ac 'ZFILES DESKTOP ready items=5 ' /tmp/zdesktop.log" | tail -1)
			[ "${now:-0}" -gt "${before:-0}" ] 2>/dev/null && break
			tries=$((tries + 1))
			sleep 1
		done
		[ "${now:-0}" -gt "${before:-0}" ] 2>/dev/null && echo "overflow gone: ready items=5 ok" || { echo "overflow gone: ready items=5 MISSING"; status=1; }
		;;
	menu)
		# The desktop surface is at y=44 (under the system bar): a screen point's surface y is 44 less.  Icons from the top right:
		# Projects (1216,90), notes.txt (1216,194), photo.png (1216,298), report.pdf (1216,402), script.sh (1216,506).
		# Rows are numbered 1000 + the action: New Folder 1002, Copy 1010, Paste 1011, Rename 1014, Show in Files 1055,
		# Clean Up 1056, Change Wallpaper 1057.
		# A fresh desktop and no saved places, the compositor started again with Files (T1-261: the prune step had left
		# notes.txt at column 3, row 3 and the overflow's files, so the places below were not the ones this step expects).
		guest "$stop_all" >/dev/null
		fresh_desktop
		guest 'rm -rf /tmp/dhome/.local/share/Trash /tmp/dhome/.config/keiland/desktop-layout; rm -f /tmp/files.clipboard' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/tmp/dhome; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass \$picture --desktop-client='/bin/files --desktop' > /tmp/zdesktop.log 2>&1 </dev/null &
i=0; while ! grep -aq 'ZFILES READY' /tmp/zdesktop.log && [ \$i -lt 60 ]; do sleep 0.5; i=\$((i+1)); done; sleep 2; echo started" >/dev/null
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=notes.txt column=0 row=1 '
		rclick 700 400
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP context empty x=700 y=356'
		expect_log /tmp/zdesktop.log 'KWL MENU row item=1056 depth=1 '
		expect_log /tmp/zdesktop.log 'KWL MENU row item=1055 depth=1 '
		shot menu-empty.png
		choose 1002
		expect_log /tmp/zdesktop.log 'ZFILES NEWFOLDER path=/tmp/dhome/Desktop/untitled folder'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=untitled folder column=0 row=5 '
		sleep 1
		shot rename-field.png
		keys 'Plans' '<ret>'
		expect_log /tmp/zdesktop.log 'ZFILES RENAME from=/tmp/dhome/Desktop/untitled folder to=/tmp/dhome/Desktop/Plans'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP rename from=untitled folder to=Plans error=0'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=Plans column=0 row=5 '
		# notes.txt's menu: Rename; the stem is selected, so "todo" makes todo.txt, which keeps the cell.
		rclick 1216 194
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP context name=notes.txt'
		expect_log /tmp/zdesktop.log 'KWL MENU row item=1014 depth=1 '
		shot menu-item.png
		choose 1014
		keys 'todo' '<ret>'
		expect_log /tmp/zdesktop.log 'ZFILES RENAME from=/tmp/dhome/Desktop/notes.txt to=/tmp/dhome/Desktop/todo.txt'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=todo.txt column=0 row=1 '
		# photo.png copied and pasted on the empty desktop: into its own folder the copy is "photo 2.png".
		rclick 1216 298
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP context name=photo.png'
		choose 1010
		expect_log /tmp/zdesktop.log 'ZFILES CLIPBOARD mode=[0-9]+ items=1'
		rclick 700 400
		choose 1011
		expect_log /tmp/zdesktop.log 'ZFILES TASK done id=[0-9]+ kind=copy state=done files=1 '
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP ready items=7 cells=7'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=photo 2.png column=1 row=0 '
		# Another folder's report.pdf on the clipboard (Files' clipboard file), pasted: the name is taken, the
		# question is over the desktop, and Enter keeps both.
		guest 'mkdir -p /tmp/dhome/Other; printf "other\n" > /tmp/dhome/Other/report.pdf; printf "copy\n/tmp/dhome/Other/report.pdf\n" > /tmp/files.clipboard' >/dev/null
		rclick 700 400
		choose 1011
		expect_log /tmp/zdesktop.log 'ZFILES DIALOG collision index=0 name=report.pdf'
		sleep 1
		shot collision.png
		keys '<ret>'
		expect_log /tmp/zdesktop.log 'ZFILES COLLISION answer='
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP ready items=8 cells=8'
		# script.sh to the trash with Delete (the ready line of 7 items again, after the 8).
		click 1216 506
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP select name=script.sh selected=1'
		keys '<delete>'
		sleep 2
		sevens=$(guest "grep -ac 'ZFILES DESKTOP ready items=7 cells=7' /tmp/zdesktop.log" | tail -1)
		[ "${sevens:-0}" -ge 2 ] 2>/dev/null && echo "trash: 7 items again ok" || { echo "trash: 7 items again MISSING"; status=1; }
		trashed=$(guest 'ls /tmp/dhome/.local/share/Trash/files/ 2>/dev/null | grep -c "^script.sh"' | tail -1)
		[ "${trashed:-0}" -ge 1 ] 2>/dev/null && echo "trash: script.sh in the trash ok" || { echo "trash: script.sh MISSING"; status=1; }
		sleep 1
		shot trash.png
		# Show in Files from report.pdf's menu: a Files window on ~/Desktop, closed by Ctrl+W.
		maps=$(guest "grep -ac 'KWL MAP client=' /tmp/zdesktop.log" | tail -1)
		rclick 1216 402
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP context name=report.pdf'
		choose 1055
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP show-in-files path=/tmp/dhome/Desktop error=0'
		sleep 4
		now=$(guest "grep -ac 'KWL MAP client=' /tmp/zdesktop.log" | tail -1)
		[ "${now:-0}" -gt "${maps:-0}" ] 2>/dev/null && echo "show-in-files: a window ok" || { echo "show-in-files: no window MISSING"; status=1; }
		pointer move 640 780 sleep 300
		shot show-in-files.png
		# With that window on top, the desktop's menu (at 40,500, left of the window) stays open, and closes without a
		# choice at a press on the window, and when another window maps.
		done=$(guest "grep -ac 'KWL MENU context-done' /tmp/zdesktop.log" | tail -1)
		rclick 40 500
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP context empty x=40 y=456'
		sleep 1
		open=$(guest "grep -ac 'KWL MENU context-done' /tmp/zdesktop.log" | tail -1)
		[ "${open:-0}" = "${done:-0}" ] && echo "desktop menu over a window: stays open ok" || { echo "desktop menu over a window: closed MISSING"; status=1; }
		shot menu-over-window.png
		click 700 500
		now=$(guest "grep -ac 'KWL MENU context-done' /tmp/zdesktop.log" | tail -1)
		[ "${now:-0}" -gt "${open:-0}" ] 2>/dev/null && echo "desktop menu: a press on the window closes it ok" || { echo "desktop menu: press on the window MISSING"; status=1; }
		rclick 40 500
		open=$(guest "grep -ac 'KWL MENU context-done' /tmp/zdesktop.log" | tail -1)
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/tmp/dhome; /bin/files --token=m --timeout-s=300 --width=600 --height=400 /tmp/dhome > /tmp/f3.log 2>&1 </dev/null & sleep 6; echo started" >/dev/null
		now=$(guest "grep -ac 'KWL MENU context-done' /tmp/zdesktop.log" | tail -1)
		[ "${now:-0}" -gt "${open:-0}" ] 2>/dev/null && echo "desktop menu: a new window closes it ok" || { echo "desktop menu: new window MISSING"; status=1; }
		keys '<ctrl-w>'
		sleep 1
		keys '<ctrl-w>'
		sleep 1
		# Clean Up: the items in order down the first column again (the trashed item's cell is taken back), the seventh at
		# the top of the second column: the 756-pixel desktop under the 44-pixel bar has six rows (ws099-p031).
		rclick 700 400
		choose 1056
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP clean-up error=0'
		sleep 1
		rows=$(guest "awk '/DESKTOP clean-up/{f=1} f && /DESKTOP place/' /tmp/zdesktop.log | sed -n 's/.* column=\\([0-9]*\\) row=\\([0-9]*\\) .*/\\1,\\2/p' | sort | tr '\\n' ' '" | tail -1)
		[ "$rows" = "0,0 0,1 0,2 0,3 0,4 0,5 1,0 " ] && echo "clean-up: in order ok" || { echo "clean-up: ($rows) MISSING"; status=1; }
		pointer move 640 600 sleep 300
		shot cleanup.png
		# Change Wallpaper, when the menu has it (Settings installed).
		rclick 700 400
		wallpaper=$(guest "grep -ac 'MENU row item=1057 ' /tmp/zdesktop.log" | tail -1)
		if [ "${wallpaper:-0}" -gt 0 ] 2>/dev/null; then
			choose 1057
			expect_log /tmp/zdesktop.log 'ZFILES DESKTOP change-wallpaper error=0'
			sleep 5
			shot wallpaper.png
			guest 'for p in $(ps -A -o pid,args | grep "[s]ettings" | awk "{print \$1}"); do kill $p; done' >/dev/null
		else
			keys '<esc>'
			echo "change-wallpaper: no Settings on the image (not offered)"
		fi
		;;
	drag)
		# From the five items of install and no saved layout, whatever ran before (menu renames, moves and trashes
		# items; T1-097 ran drag after menu): the compositor started again with Files.
		guest "$stop_all" >/dev/null
		fresh_desktop
		guest 'rm -f /tmp/dhome/.config/keiland/desktop-layout' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/tmp/dhome; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass \$picture --desktop-client='/bin/files --desktop' > /tmp/zdesktop.log 2>&1 </dev/null &
i=0; while ! grep -aq 'ZFILES READY' /tmp/zdesktop.log && [ \$i -lt 60 ]; do sleep 0.5; i=\$((i+1)); done; sleep 2; echo started" >/dev/null
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=notes.txt column=0 row=1 '
		# A cell's column counts from the right edge: the surface point (x, y - 44) is in column
		# (1280 - 16 - x - 1) / 96 and row (y - 44 - 16) / 104.
		# 1. notes.txt (1216,194) to (900,400): the cell 3,3.
		pointer move 1216 194 sleep 300 down sleep 150 move 1205 200 sleep 100 move 1150 240 sleep 100 move 1050 300 sleep 100 \
			move 950 370 sleep 100 move 900 400 sleep 900
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP drag start name=notes.txt'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP drop enter self=1 files=1'
		shot drag-over.png
		pointer up sleep 1500
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP drop place column=3 row=3'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP move name=notes.txt column=3 row=3 error=0'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP drag done'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=notes.txt column=3 row=3 '
		pointer move 640 600 sleep 300
		shot drag-moved.png
		# 2. photo.png (1216,298) onto the folder Projects (1216,90): moved into it.
		pointer move 1216 298 sleep 300 down sleep 150 move 1210 290 sleep 100 move 1200 220 sleep 100 move 1210 140 sleep 100 \
			move 1216 95 sleep 900 up sleep 2000
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP drop target=/tmp/dhome/Desktop/Projects'
		expect_log /tmp/zdesktop.log 'ZFILES DROP operation=move items=1 destination=/tmp/dhome/Desktop/Projects'
		moved=$(guest 'ls /tmp/dhome/Desktop/Projects/photo.png /tmp/dhome/Desktop/photo.png 2>/dev/null | tr "\n" " "' | tail -1)
		[ "$moved" = "/tmp/dhome/Desktop/Projects/photo.png " ] && echo "folder: photo.png moved into Projects ok" || { echo "folder: ($moved) MISSING"; status=1; }
		# 3. A Files window on ~/Docs (list view): its note.txt dragged out to (60,600), the cell 12,5.
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/tmp/dhome; mkdir -p /tmp/dhome/Docs; echo hello > /tmp/dhome/Docs/note.txt; /bin/files --token=f2 --timeout-s=800 --width=700 --height=500 /tmp/dhome/Docs > /tmp/f2.log 2>&1 </dev/null & sleep 6; echo started" >/dev/null
		expect_log /tmp/f2.log 'ZFILES READY'
		keys '<ctrl-2>'
		set -- $(guest "grep -a 'KWL MAP client=' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
		wx=${1:-0}; wy=${2:-0}
		echo "files window at $wx,$wy"
		pointer move $((wx + 400)) $((wy + 114)) sleep 300 down sleep 100 move $((wx + 380)) $((wy + 120)) sleep 80 move $((wx + 300)) $((wy + 200)) sleep 80 \
			move $((wx - 40)) 500 sleep 150 move 100 580 sleep 150 move 60 600 sleep 900
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP drop enter self=0 files=1'
		shot drop-in.png
		pointer up sleep 2000
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP drop self=0 destination=/tmp/dhome/Desktop action=[0-9]+ column=12 row=5'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP dropped name=note.txt column=12 row=5 error=0'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=note.txt column=12 row=5 '
		there=$(guest 'ls /tmp/dhome/Desktop/note.txt 2>/dev/null' | tail -1)
		[ "$there" = /tmp/dhome/Desktop/note.txt ] && echo "drop in: note.txt in ~/Desktop ok" || { echo "drop in: note.txt MISSING"; status=1; }
		# 4. The desktop's report.pdf (1216,402) onto the Files window: moved into ~/Docs.
		pointer move 1216 402 sleep 300 down sleep 150 move 1200 400 sleep 100 move 1100 380 sleep 100 move $((wx + 500)) $((wy + 300)) sleep 150 \
			move $((wx + 350)) $((wy + 300)) sleep 900
		shot drop-out.png
		pointer up sleep 2000
		expect_log /tmp/f2.log 'ZFILES DROP operation=move items=1 destination=/tmp/dhome/Docs'
		there=$(guest 'ls /tmp/dhome/Docs/report.pdf 2>/dev/null' | tail -1)
		[ "$there" = /tmp/dhome/Docs/report.pdf ] && echo "drop out: report.pdf in ~/Docs ok" || { echo "drop out: report.pdf MISSING"; status=1; }
		pointer move $((wx + 350)) $((wy + 300)) sleep 200 down sleep 60 up sleep 500
		keys '<ctrl-w>'
		sleep 1
		;;
	touch)
		# 1. A double tap on Projects (1216,90): a new Files window on it, closed by Ctrl+W.
		touch_replay tap-projects 'size 1279 799 2
wait 2600
down 1 1216 90
wait 60
up 1
wait 120
down 1 1216 90
wait 60
up 1
hold 2500'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP open name=Projects via=double-click'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP open-folder path=/tmp/dhome/Desktop/Projects error=0'
		sleep 2
		keys '<ctrl-w>'
		sleep 1
		# 2. A long press on report.pdf (1216,402): its context menu.
		touch_replay press-report 'size 1279 799 2
wait 2600
down 1 1216 402
hold 1000
up 1
hold 1500'
		expect_log /tmp/zdesktop.log 'ZFILES TOUCH long-press'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP context name=report.pdf'
		expect_log /tmp/zdesktop.log 'ZFILES CONTEXT-MENU open rows=[0-9]+ x=1216 y=358 '
		shot touch-menu.png
		keys '<esc>'
		sleep 1
		# 3. A long press on script.sh (1216,506) that moves to (800,500): dragged to the cell 4,4.
		touch_replay drag-script 'size 1279 799 2
wait 2600
down 1 1216 506
hold 1000
swipe -416 -6 26 16
hold 600
up 1
hold 2000'
		expect_log /tmp/zdesktop.log 'ZFILES TOUCH hold'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP drag start name=script.sh'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP move name=script.sh column=4 row=4 error=0'
		expect_log /tmp/zdesktop.log 'ZFILES DESKTOP place name=script.sh column=4 row=4 '
		pointer move 640 700 sleep 300
		shot touch-moved.png
		;;
	stop)
		guest "$stop_all" >/dev/null
		;;
	perf100)
		# ws094-p008 (L3): the desktop with 100 items (20 pictures, 10 folders, 70 texts in /tmp/dhome100/Desktop), three
		# rounds, each on a compositor started afresh: (a) zdesktop's start of files --desktop (KWL DESKTOP start at_ms)
		# to ZFILES DESKTOP ready items=100 (at_ms); (b) a file added to the ready desktop: the age of the newest item when
		# it is shown (ready items=101 newest_age_ms); (c) three clicks on items: the press to the selection's frame
		# (DESKTOP select-frame ms).  The medians against L3's targets (PERF_START_MS, PERF_ADDED_MS, PERF_SELECT_MS),
		# and the SLOW-FRAME lines counted (perf100.txt, perf100.png).  ws094-p009: (a') zdesktop's first drawing of the
		# desktop's image (KWL DESKTOP drawn at_ms), and the steps of Files' start (perf100-steps.txt).
		start_limit=${PERF_START_MS:-1500}; added_limit=${PERF_ADDED_MS:-2500}; select_limit=${PERF_SELECT_MS:-50}
		python3 plan/tools/imageview/make-images.py build/ws094-images >/dev/null
		guest 'rm -rf /tmp/dhome100; mkdir -p /tmp/dhome100/Desktop /tmp/p100' >/dev/null
		for picture in 01-splash.png 02-landscape.jpg 03-portrait.jpg 04-mark.png 05-anim.gif 06-pixels.png; do
			put "build/ws094-images/$picture" "/tmp/p100/$picture"
		done
		guest 'cd /tmp/p100; n=1; while [ $n -le 20 ]; do for f in *; do [ $n -le 20 ] || break; e=${f##*.}; cp "$f" /tmp/dhome100/Desktop/picture-$n.$e; n=$((n+1)); done; done
n=1; while [ $n -le 10 ]; do mkdir /tmp/dhome100/Desktop/folder-$n; n=$((n+1)); done
n=1; while [ $n -le 70 ]; do printf "note %d\n" $n > /tmp/dhome100/Desktop/note-$n.txt; n=$((n+1)); done
ls /tmp/dhome100/Desktop | wc -l' | tail -1 | sed 's/^/items made: /'
		: > "$out/perf100.txt"
		for round in 1 2 3; do
			guest "$stop_all" >/dev/null
			guest "export XDG_RUNTIME_DIR=/tmp HOME=/tmp/dhome100; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=900 --width=1280 --height=800 --glass \$picture --desktop-client='/bin/files --desktop' > /tmp/zdesktop.log 2>&1 </dev/null &
i=0; while ! grep -aq 'ZFILES DESKTOP ready items=100 ' /tmp/zdesktop.log && [ \$i -lt 120 ]; do sleep 0.25; i=\$((i+1)); done; sleep 3; echo started" >/dev/null
			began=$(guest "grep -a 'KWL DESKTOP start ' /tmp/zdesktop.log | tail -1" | sed -n 's/.* at_ms=\([0-9]*\).*/\1/p')
			ready=$(guest "grep -a 'ZFILES DESKTOP ready items=100 ' /tmp/zdesktop.log | head -1" | sed -n 's/.* at_ms=\([0-9]*\).*/\1/p')
			start_ms=$(( ${ready:-0} - ${began:-0} ))
			[ -n "$began" ] && [ -n "$ready" ] || start_ms=-1
			# (a') zdesktop's first drawing of the desktop's image (ws094-p009): what the screen shows, not only Files' frame.
			drawn=$(guest "grep -a 'KWL DESKTOP drawn ' /tmp/zdesktop.log | tail -1" | sed -n 's/.* at_ms=\([0-9]*\).*/\1/p')
			drawn_ms=$(( ${drawn:-0} - ${began:-0} ))
			[ -n "$began" ] && [ -n "$drawn" ] || drawn_ms=-1
			guest "printf 'new\n' > /tmp/dhome100/Desktop/added-$round.txt" >/dev/null
			tries=0; line=
			while [ $tries -lt 20 ] && [ -z "$line" ]; do
				sleep 0.5; tries=$((tries + 1))
				line=$(guest "grep -a 'ZFILES DESKTOP ready items=101 ' /tmp/zdesktop.log | head -1")
			done
			added_ms=$(printf '%s\n' "$line" | sed -n 's/.* newest_age_ms=\([-0-9]*\).*/\1/p')
			[ -n "$added_ms" ] || added_ms=-1
			selects=""
			for y in 90 194 298; do
				pointer move 1216 $y sleep 300 down sleep 60 up sleep 800
			done
			sleep 1
			selects=$(guest "grep -a 'ZFILES DESKTOP select-frame ms=' /tmp/zdesktop.log" | sed -n 's/.* ms=\([0-9]*\).*/\1/p' | tr '\n' ' ')
			slow=$(guest "grep -ac 'ZFILES SLOW-FRAME' /tmp/zdesktop.log" | tail -1)
			echo "round=$round start_ms=$start_ms drawn_ms=$drawn_ms added_ms=$added_ms select_ms=$selects slow_frames=${slow:-?}" | tee -a "$out/perf100.txt"
			# The steps of the start (ws094-p009): exec is from zdesktop's start to Files' main(), the rest from Files' log.
			steps=$(guest "grep -a 'ZFILES DESKTOP startup ' /tmp/zdesktop.log | head -1")
			entered=$(printf '%s\n' "$steps" | sed -n 's/.* entered_ms=\([0-9]*\).*/\1/p')
			[ -n "$entered" ] && [ -n "$began" ] && echo "steps round=$round exec=$((entered - began)) $(printf '%s\n' "$steps" | sed 's/.* entered_ms=[0-9]* //')" | tee -a "$out/perf100-steps.txt"
			[ $round = 1 ] && shot perf100.png
			guest "grep -a 'SLOW-FRAME' /tmp/zdesktop.log" >> "$out/perf100-slow.txt"
			guest "rm -f /tmp/dhome100/Desktop/added-$round.txt" >/dev/null
		done
		# The medians of the rounds (for (c), of every click) against the targets.
		python3 - "$out/perf100.txt" "$start_limit" "$added_limit" "$select_limit" <<'PY' | tee -a "$out/perf100.txt"
import re, statistics, sys
text = open(sys.argv[1]).read()
starts = [int(v) for v in re.findall(r"start_ms=(-?\d+)", text)]
drawns = [int(v) for v in re.findall(r"drawn_ms=(-?\d+)", text)]
added = [int(v) for v in re.findall(r"added_ms=(-?\d+)", text)]
selects = [int(v) for part in re.findall(r"select_ms=([\d ]*) slow", text) for v in part.split()]
slow = sum(int(v) for v in re.findall(r"slow_frames=(\d+)", text))
def median(values):
    return int(statistics.median(values)) if values and min(values) >= 0 else -1
a, b, c = median(starts), median(added), median(selects)
limits = [int(v) for v in sys.argv[2:5]]
print("RESULT start_ms=%d drawn_ms=%d added_ms=%d select_ms=%d slow_frames=%d (targets %d %d %d, slow 0)" % (a, median(drawns), b, c, slow, *limits))
for name, value, limit in (("a start", a, limits[0]), ("b added", b, limits[1]), ("c select", c, limits[2])):
    print("L3 %s: %d ms %s" % (name, value, "within" if 0 <= value <= limit else "OVER"))
print("L3 slow frames: %d %s" % (slow, "none" if slow == 0 else "SOME"))
PY
		;;
	*)
		echo "unknown step $step"
		status=1
		;;
	esac
done
errors=$(guest "grep -ac ERROR /tmp/zdesktop.log" | tail -1)
[ "${errors:-1}" = 0 ] && echo "zdesktop: no ERROR" || { echo "zdesktop: ERROR lines"; status=1; }
failed=$(guest "grep -ac 'ZFILES FAILED' /tmp/zdesktop.log" | tail -1)
[ "${failed:-1}" = 0 ] && echo "files: no FAILED" || { echo "files: FAILED lines"; status=1; }
guest 'grep -a "DESKTOP" /tmp/zdesktop.log' > "$out/desktop-log.txt"
[ $status = 0 ] && echo "files-desktop-guest: PASS" || echo "files-desktop-guest: FAIL"
exit $status
