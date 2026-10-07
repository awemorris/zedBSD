#!/bin/sh
# ws102: the on-screen keyboard (userland/desktop/wayland/keyboard.c) on the Venus guest.  The running guest gets this
# worktree's compositor (BIN); zdesktop --glass at 1280x800 without a client.  The steps read zdesktop's log through SSH
# and the pictures (nothing reads the console).  The pointer is driven through QMP, the fingers with /bin/touchinject
# (the pen image's injected touch screen, 1280x800 screen pixels).
#   install   the compositor and its libraries into the guest
#   start     zdesktop --glass 1280x800 (started again by each step that needs a fresh one)
#   pointer   (p002) the pointer's swipes: the bottom-right corner's opens the flick panel (318x756 at 962,44, the right column;
#             flick-open.png), its close key closes it; the same swipe twice opens and closes it; the bottom-left
#             corner's opens the QWERTY panel (1280x336 at 0,464, the bottom row; qwerty-open.png) and the bottom-right's then changes
#             to the flick panel
#   edges     (p002, D3) the corners do not take the other gestures' strokes: a straight-up stroke from the bottom-right
#             corner opens nothing; the bottom edge's swipe up in the middle still opens Wiseview; a swipe right from
#             the left edge just above the corner still switches the desktop
#   flick     (p003) the flick panel's keys (72 px at 968,488 and every 78 px): a tap on あ, a flick left on か (き),
#             a flick up held on な (the petals, petals.png; ぬ), the face key to the alpha face (abc: a, flick up c) and
#             the number face (1, 2 flicked down >), back to kana; a finger's flick right on あ (え)
#   send      (p004) what the keys type reaches the focused application: in Text Editor (/root/osk.txt) the alpha face's
#             a i u e o, the case key (O) and the number face's 1 2 3 as keys, saved by Ctrl+S: the file is
#             "aiueO123"; in ime-probe (a text input) the kana face's あ い う え お and か with the voice key (が)
#             as commits: its text is "あいうえおかが" with a deletion of 3 bytes before が; in wltest (no text input)
#             a kana is refused; in Text Editor a kana (WS090's text input) is also tried, and noted
#   close     (p005) the title band dragged 100 px right closes the flick panel, 100 px down the QWERTY panel; App
#             Home and Wiseview close an open panel (the lock screen needs a session's compositor: not checked here);
#             (BUG-229) they put it away (put-away), and without a focused window it is not brought back
#             (restore-dropped reason=focus); with Text Editor focused, App Home's Esc brings it back (restore,
#             restored.png)
#   hint      (BUG-230) the bottom-right corner's hint held without letting go: the quarter disc of glass from the corner
#             to the contact, 40 px along the diagonal the disc only (hint-short.png), 80 px with "Keyboard" fading in
#             (hint-label.png), 130 px with the label and the blue rim (hint-ready.png); letting go opens the flick panel
#   large     (p005; the guest started with VENUS_SIZE=1920x1080, OSK_WIDTH=1920 OSK_HEIGHT=1080) the flick panel is
#             414x1036 at 1506,44 (keys 96 px), the QWERTY panel 1920x453 at 0,627 (large-flick.png, large-qwerty.png)
#   qwerty    (p006) the QWERTY panel: the 30 characters "Hello, World! Kei 2026 (a+b)=c" (capitals by Shift, symbols
#             by Shift on the digits and by the symbols face) tapped into Text Editor within 6 s (5 characters a second,
#             qwerty-plan.py), saved by Ctrl+S: the file is the text; Shift twice locks it (ABC typed as capitals);
#             an arrow key moves the caret (qwerty.png, qwerty-symbols.png)
#   qwerty-ime (BUG-231, p025; the input method's image, plan/ws095/tests/build-ime-image.sh: keiland-ime and its
#             dictionary) Japanese on (Alt+Space) with ime-probe focused, the QWERTY panel's keys go to the input method:
#             "a" is the preedit あ ("KWL OSK send via=ime code=30", the probe's preedit=あ), space converts (code=57,
#             qwerty-ime.png shows the candidates), Enter commits (code=28): the probe's text is not "a" nor empty
#   hand      (p008) the QWERTY panel's band button (1144,472 84x28) opens the handwriting face (writing area 962x288 at
#             6,506): two strokes of the pointer and one of a finger are drawn (hand.png), the recognizer (ws165-p003,
#             the hand-hershey templates) gives 1 to 4 candidates 600 ms after the last, without a note; every frame that
#             draws new points does so within one frame of their input (the lag logged by zdesktop); the first
#             candidate, whatever it is, is sent to ime-probe; clear empties the ink; the band button goes back to the keys
#   extra     (p020) the QWERTY panel's extra keys' row: in Text Editor "bc", Home, "a", End, "|" (from the extra
#             row), Tab: "abc|<tab>"; then Ctrl (held for one key) and a select all, "z" replaces it: the file is "z";
#             Alt then Esc lets go of Alt with the key (extra.png)
#   roll      (p009; the pen image) two fingers typing on the QWERTY panel into Text Editor (/root/roll.txt): f and j
#             in turn, 100 taps, each finger touching 50 ms before the other lifts (touch.c's ROUTE_OSK: each finger its
#             own press, the second's touch makes the first's key act); the file is "fj" 50 times, 100 keys logged,
#             99 rollovers, none lost (roll.png)
#   tools     (p016) the flick panel's tools: select, right x5, copy, the application before, paste, three times, from
#             one Text Editor into another: "hellohellohello"
#   history   (p024) the history tab: three copies ("alpha", "bravo", "charlie"), the second row pasted: "bravo"
#   emoji     (p022) the emoji tab (1238,141): category 0's first emoji and category 1's fourth reach ime-probe as
#             commits (PROBE TEXT); wltest (no text input) refuses one (sent=0); one tapped into Text Editor and saved
#             is its UTF-8 (od); emoji.png, emoji-sent.png.  The cells' places are read from the log (KWL OSK erect)
#   touch     (p002; the pen image) 10 injected swipes from the bottom-right corner open and close the panel 10 times
#             (5 opens, 5 closes); 10 straight-up strokes from the corner open nothing
#   OUTDIR is the first argument:  GUEST_RUNTIME=... BIN=build/ws102-amd64 plan/ws102/tests/osk-guest.sh OUTDIR STEP...
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws102-run}"
bin=${BIN:-build/ws102-amd64}
out=$1
shift
mkdir -p "$out"
status=0
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
put() { timeout 120 python3 plan/tools/guest/guest.py put "$1" "$2" >/dev/null 2>&1 </dev/null || { echo "put $1: FAILED"; status=1; }; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py --width "${OSK_WIDTH:-1280}" --height "${OSK_HEIGHT:-800}" "$GUEST_RUNTIME/qmp.sock" "$@"; }
shot() {
	python3 plan/ws035/tests/zdesktop-check.py "$out/$1" --runtime "$GUEST_RUNTIME" >/dev/null
	echo "shot $1"
}
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)|[f]iles|[w]ltest" | awk "{print \$1}"); do kill $p; done; sleep 1'

# Reads the last line of a guest file, again when the read comes back empty (an SSH read can fail now and then).
read_file() {
	tries=0
	line=
	while [ $tries -lt 4 ]; do
		line=$(guest "cat $1" | tail -1)
		[ -n "$line" ] && break
		tries=$((tries + 1))
		sleep 1
	done
	printf '%s\n' "$line"
}

# Counts a log's lines matching a pattern.
count() {
	found=$(guest "grep -acE '$1' /tmp/zdesktop.log" | tail -1)
	echo "${found:-0}"
}

# Fails the run unless zdesktop's log has a line matching a pattern (within a few seconds).
expect_log() {
	tries=0
	found=0
	while [ $tries -lt 8 ]; do
		found=$(count "$1")
		[ "$found" -gt 0 ] 2>/dev/null && break
		tries=$((tries + 1))
		sleep 1
	done
	if [ "$found" -gt 0 ] 2>/dev/null; then
		echo "log: $1 ok"
	else
		echo "log: $1 MISSING"
		status=1
	fi
}

# Fails the run unless zdesktop's keyboard lines have a fixed text (compared on the host: UTF-8 through the guest's
# shell is not reliable).
expect_text() {
	tries=0
	found=0
	while [ $tries -lt 8 ]; do
		guest "grep -a 'KWL OSK' /tmp/zdesktop.log" > "$out/osk-now.txt"
		found=$(grep -cF "$1" "$out/osk-now.txt")
		[ "$found" -gt 0 ] 2>/dev/null && break
		tries=$((tries + 1))
		sleep 1
	done
	if [ "$found" -gt 0 ] 2>/dev/null; then
		echo "text: $1 ok"
	else
		echo "text: $1 MISSING"
		status=1
	fi
}

# Fails the run unless a pattern's count is exactly the one given.
expect_count() {
	found=$(count "$1")
	if [ "$found" = "$2" ]; then
		echo "count: $1 = $2 ok"
	else
		echo "count: $1 = $found (expected $2) MISSING"
		status=1
	fi
}

# Starts zdesktop afresh: glass, 1280x800 (OSK_WIDTH and OSK_HEIGHT for another size), no client.
compositor() {
	guest "$stop_all" >/dev/null
	guest "export XDG_RUNTIME_DIR=/tmp; rm -f /tmp/wayland-0; picture=; [ -f /usr/share/keiland/wallpaper.png ] && picture=--wallpaper=/usr/share/keiland/wallpaper.png
/bin/wayland --testing --timeout=900 --width=${OSK_WIDTH:-1280} --height=${OSK_HEIGHT:-800} --glass \$picture > /tmp/zdesktop.log 2>&1 </dev/null &
i=0; while ! grep -q 'KWL OSK zone' /tmp/zdesktop.log && [ \$i -lt 60 ]; do sleep 0.5; i=\$((i+1)); done; sleep 1; echo started" >/dev/null
}

# Replays a touch script (screen pixels of 1280x800).
touch_replay() {
	printf '%s\n' "$2" > "$out/$1.script"
	put "$out/$1.script" "/tmp/$1.script"
	result=$(guest "/bin/touchinject /tmp/$1.script 2>&1; echo replay=\$?")
	printf '%s\n' "$result" | grep -q '^replay=0$' || { echo "touchinject $1: FAILED"; status=1; }
}

# Reads the QWERTY keys' places from zdesktop's log once (qkey_tap uses them).
qkey_refresh() {
	tries=0
	while [ $tries -lt 5 ]; do
		guest "grep -a 'KWL OSK qrect' /tmp/zdesktop.log" > "$out/qrect-now.txt"
		grep -q 'KWL OSK qrect' "$out/qrect-now.txt" && break
		tries=$((tries + 1))
		sleep 1
	done
}

# A tap (twice quickly when the second argument is 2) on the QWERTY key with a label, at its latest place read by
# qkey_refresh; a label not found fails the run and taps nothing.
qkey_tap() {
	place=$(awk -v l="label=$1" '$NF == l' "$out/qrect-now.txt" | tail -1 | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
	taps=${2:-1}
	if [ -z "$place" ]; then
		echo "qkey $1: no place MISSING"
		status=1
		return
	fi
	set -- $place
	if [ "$taps" = 2 ]; then
		pointer move $(( $1 + $3 / 2 )) $(( $2 + $4 / 2 )) sleep 150 down sleep 40 up sleep 120 down sleep 40 up sleep 400
	else
		pointer move $(( $1 + $3 / 2 )) $(( $2 + $4 / 2 )) sleep 150 down sleep 60 up sleep 400
	fi
}

# A tap on a tool of the flick panel.
tool_tap() { pointer move "$1" "$2" sleep 120 down sleep 50 up sleep 350; }

# A tap on a key, and a flick of one (dx, dy) in steps.
key_tap() { pointer move "$1" "$2" sleep 150 down sleep 60 up sleep 350; }
key_flick() {
	pointer move "$1" "$2" sleep 150 down sleep 50 move $(( $1 + $3 / 2 )) $(( $2 + $4 / 2 )) sleep 50 move $(( $1 + $3 )) $(( $2 + $4 )) sleep 80 up sleep 350
}

# A swipe of the pointer from one point to another in steps, the button held.
swipe() {
	pointer move "$1" "$2" sleep 200 down sleep 80 move $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 )) sleep 60 move "$3" "$4" sleep 120 up sleep 700
}

for step in "$@"; do
	case "$step" in
	install)
		put "$bin/bin/wayland" /bin/wayland
		put userland/desktop/fonts/DroidSansFallbackFull.ttf /usr/share/fonts/keiland-fallback.ttf
		for program in textedit ime-probe wltest; do
			[ -f "$bin/bin/$program" ] && put "$bin/bin/$program" "/bin/$program"
		done
		guest 'chmod 755 /bin/textedit /bin/ime-probe /bin/wltest 2>/dev/null' >/dev/null
		for library in $(cd "$bin/dynamic" && ls *.so | grep -vE '^(libc|ld)\.so$'); do
			put "$bin/dynamic/$library" "/lib/$library"
		done
		# The colour emoji font (ws102-p019), which the compositor and libkeiland open.
		emoji=build/distfiles/NotoColorEmoji-2.047.ttf
		[ -f "$emoji" ] && put "$emoji" /usr/share/fonts/keiland-emoji.ttf
		# The handwriting templates (ws165-p003, the package hand-hershey: make hand-hershey).
		hand=build/packages/hand-hershey/hershey.txt
		[ -f "$hand" ] && guest 'mkdir -p /usr/share/keiland/hand' >/dev/null && put "$hand" /usr/share/keiland/hand/hershey.txt
		guest 'chmod 755 /bin/wayland' >/dev/null
		;;
	start)
		compositor
		expect_log 'KWL OSK zone kind=flick x=1252 y=772 size=28'
		expect_log 'KWL OSK zone kind=qwerty x=0 y=772 size=28'
		;;
	pointer)
		compositor
		# The bottom-right corner's swipe up and left: the flick panel.
		swipe 1272 792 1130 650
		expect_log 'KWL OSK press corner=flick source=pointer x=1272 y=792'
		expect_log 'KWL OSK armed corner=flick'
		expect_log 'KWL OSK commit corner=flick via=(distance|flick)'
		expect_log 'KWL OSK open kind=flick x=962 y=44 width=318 height=756'
		pointer move 700 300 sleep 400
		shot flick-open.png
		# Its close key (1228,442 28x28).
		pointer move 1254 56 sleep 200 down sleep 60 up sleep 600
		expect_log 'KWL OSK close kind=flick reason=key'
		# The same swipe twice: open, then closed by the gesture.
		swipe 1272 792 1130 650
		swipe 1272 792 1130 650
		expect_log 'KWL OSK close kind=flick reason=gesture'
		# The bottom-left corner's swipe up and right: the QWERTY panel.
		swipe 6 792 150 650
		expect_log 'KWL OSK press corner=qwerty source=pointer x=6 y=792'
		expect_log 'KWL OSK open kind=qwerty x=0 y=464 width=1280 height=336'
		pointer move 700 200 sleep 400
		shot qwerty-open.png
		# The bottom-right corner's swipe changes to the flick panel; its close key closes it.
		swipe 1272 792 1130 650
		expect_count 'KWL OSK open kind=flick' 3
		pointer move 1254 56 sleep 200 down sleep 60 up sleep 600
		expect_count 'KWL OSK close kind=flick reason=key' 2
		;;
	flick)
		compositor
		swipe 1272 792 1130 650
		expect_log 'KWL OSK open kind=flick x=962 y=44'
		# A tap on あ, a flick left on か.
		pointer move 1004 524 sleep 200 down sleep 80 up sleep 500
		expect_text 'KWL OSK key face=kana row=0 column=0 dir=center action=0 text=あ'
		pointer move 1082 524 sleep 200 down sleep 60 move 1062 524 sleep 60 move 1042 526 sleep 80 up sleep 500
		expect_text 'KWL OSK key face=kana row=0 column=1 dir=left action=0 text=き'
		# A flick up held on な: its petals, then ぬ.
		pointer move 1082 602 sleep 200 down sleep 60 move 1082 587 sleep 60 move 1083 568 sleep 600
		shot petals.png
		pointer up sleep 500
		expect_text 'KWL OSK key face=kana row=1 column=1 dir=up action=0 text=ぬ'
		# The face key: the alpha face; abc tapped (a) and flicked up (c).
		pointer move 1238 758 sleep 200 down sleep 60 up sleep 500
		expect_log 'KWL OSK face name=alpha'
		pointer move 1082 524 sleep 200 down sleep 60 up sleep 500
		expect_log 'KWL OSK key face=alpha row=0 column=1 dir=center action=0 text=a'
		pointer move 1082 524 sleep 200 down sleep 60 move 1082 507 sleep 60 move 1082 490 sleep 80 up sleep 500
		expect_log 'KWL OSK key face=alpha row=0 column=1 dir=up action=0 text=c'
		pointer move 700 300 sleep 400
		shot alpha.png
		# The number face: 1 tapped, 2 flicked down (>), back to kana.
		pointer move 1238 758 sleep 200 down sleep 60 up sleep 500
		expect_log 'KWL OSK face name=number'
		pointer move 1004 524 sleep 200 down sleep 60 up sleep 500
		expect_log 'KWL OSK key face=number row=0 column=0 dir=center action=0 text=1'
		pointer move 1082 524 sleep 200 down sleep 60 move 1082 542 sleep 60 move 1082 562 sleep 80 up sleep 500
		expect_log 'KWL OSK key face=number row=0 column=1 dir=down action=0 text=>'
		pointer move 1238 758 sleep 200 down sleep 60 up sleep 500
		expect_log 'KWL OSK face name=kana'
		# A finger's flick right on あ.
		touch_replay flick-right 'size 1279 799 2
wait 2600
down 1 1004 524
swipe 40 0 8 16
up 1
hold 800'
		expect_text 'KWL OSK key face=kana row=0 column=0 dir=right action=0 text=え'
		;;
	send)
		compositor
		# Text Editor on an empty file, on top with the keyboard.
		guest 'rm -f /root/osk.txt /tmp/te.log; touch /root/osk.txt' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=600 /root/osk.txt > /tmp/te.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		expect_log 'KWL MAP client='
		swipe 1272 792 1130 650
		expect_log 'KWL OSK open kind=flick'
		# The alpha face: a (abc), i (ghi up), u (tuv left), e (def left), o (mno up), then the case key (O).
		key_tap 1238 758
		key_tap 1082 524
		key_flick 1004 602 0 -30
		key_flick 1082 680 -30 0
		key_flick 1160 524 -30 0
		key_flick 1160 602 0 -30
		key_tap 1004 758
		# The number face: 1 2 3.
		key_tap 1238 758
		key_tap 1004 524
		key_tap 1082 524
		key_tap 1160 524
		expect_log 'KWL OSK send via=key code=30 shift=0'
		expect_log 'KWL OSK send via=key code=24 shift=1'
		expect_log 'KWL OSK send via=key code=4 shift=0'
		pointer move 400 300 sleep 300
		shot send-textedit.png
		# Saved with the physical keyboard's Ctrl+S; the file read back.
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<ctrl-s>' >/dev/null
		sleep 2
		saved=$(read_file /root/osk.txt)
		[ "$saved" = "aiueO123" ] && echo "textedit: aiueO123 ok" || { echo "textedit: ($saved) MISSING"; status=1; }
		# ime-probe (a text input) on top: the kana face (the face key once more), あいうえお, か and the voice key.
		guest 'for p in $(ps -A -o pid,args | grep "[t]extedit" | awk "{print \$1}"); do kill $p; done; rm -f /tmp/ime-probe.log' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/ime-probe --log=/tmp/ime-probe.log --seconds=300 > /dev/null 2>&1 </dev/null & sleep 4; echo started" >/dev/null
		key_tap 1238 758
		key_tap 1004 524
		key_flick 1004 524 -30 0
		key_flick 1004 524 0 -30
		key_flick 1004 524 30 0
		key_flick 1004 524 0 30
		key_tap 1082 524
		key_tap 1004 758
		sleep 1
		guest 'cat /tmp/ime-probe.log' > "$out/ime-probe.log"
		grep -qF 'PROBE TEXT text=あいうえおかが' "$out/ime-probe.log" && echo "ime-probe: あいうえおかが ok" || { echo "ime-probe: text MISSING"; status=1; }
		grep -qF 'PROBE DELETE before=3 after=0' "$out/ime-probe.log" && echo "ime-probe: delete 3 before が ok" || { echo "ime-probe: delete MISSING"; status=1; }
		expect_text 'KWL OSK send via=commit text=が before=3'
		shot send-probe.png
		# wltest (no text input) on top: a kana is refused.
		guest 'for p in $(ps -A -o pid,args | grep "[i]me-probe" | awk "{print \$1}"); do kill $p; done' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp; /bin/wltest --windowed --frames=3600 > /dev/null 2>&1 </dev/null & sleep 4; echo started" >/dev/null
		key_tap 1004 524
		expect_log 'KWL OSK refused reason=no-text-input'
		guest 'for p in $(ps -A -o pid,args | grep "[w]ltest" | awk "{print \$1}"); do kill $p; done' >/dev/null
		# (noted, not required in L1) Text Editor with WS090's text input: あい, saved.
		guest 'rm -f /root/osk2.txt; touch /root/osk2.txt' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=600 /root/osk2.txt > /tmp/te2.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		key_tap 1004 524
		key_flick 1004 524 -30 0
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<ctrl-s>' >/dev/null
		sleep 2
		guest 'cat /root/osk2.txt' > "$out/osk2.txt"
		grep -qF 'あい' "$out/osk2.txt" && echo "note: Text Editor took あい by its text input" || echo "note: Text Editor did not take the kana ($(cat "$out/osk2.txt"))"
		;;
	close)
		compositor
		# The flick panel's band dragged right, the QWERTY panel's band dragged down.
		swipe 1272 792 1130 650
		pointer move 1000 52 sleep 200 down sleep 60 move 1050 52 sleep 60 move 1100 52 sleep 80 up sleep 600
		expect_log 'KWL OSK close kind=flick reason=swipe'
		swipe 6 792 150 650
		pointer move 400 480 sleep 200 down sleep 60 move 400 530 sleep 60 move 400 580 sleep 80 up sleep 600
		expect_log 'KWL OSK close kind=qwerty reason=swipe'
		# App Home (the launcher) closes the panel; Home closes by Esc.
		swipe 1272 792 1130 650
		pointer move 20 17 sleep 200 down sleep 60 up sleep 1200
		expect_log 'KWL OSK close kind=flick reason=home'
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<esc>' >/dev/null
		sleep 1.5
		# Wiseview (the bottom edge's swipe) closes it; a press closes Wiseview.
		swipe 1272 792 1130 650
		expect_count 'KWL OSK open kind=flick' 3
		swipe 640 796 640 480
		expect_log 'KWL OSK close kind=flick reason=wiseview'
		pointer move 640 400 sleep 200 down sleep 60 up sleep 1200
		# (BUG-229) both put the panel away; with no focused window it does not come back.
		expect_log 'KWL OSK put-away kind=flick reason=home'
		expect_log 'KWL OSK put-away kind=flick reason=wiseview'
		expect_count 'KWL OSK restore-dropped reason=focus' 2
		# (BUG-229) with Text Editor focused, the panel App Home put away comes back when App Home is left by Esc.
		guest ': > /root/osk3.txt' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=600 /root/osk3.txt > /tmp/te3.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		swipe 1272 792 1130 650
		pointer move 20 17 sleep 200 down sleep 60 up sleep 1200
		expect_count 'KWL OSK put-away kind=flick reason=home' 2
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<esc>' >/dev/null
		sleep 1.5
		expect_log 'KWL OSK restore kind=flick'
		shot restored.png
		guest 'for p in $(ps -A -o pid,args | grep "[t]extedit" | awk "{print \$1}"); do kill $p; done' >/dev/null
		# (The lock screen, Super+L, locks only a session's compositor (--session, sessiond); this one is not.)
		;;
	hint)
		# (BUG-230) the corner's hint, held: the disc only, the label fading in, then the label and the blue rim.
		compositor
		pointer move 1272 792 sleep 200 down sleep 80 move 1252 772 sleep 60 move 1232 752 sleep 500
		expect_log 'KWL OSK armed corner=flick'
		shot hint-short.png
		pointer move 1192 712 sleep 500
		shot hint-label.png
		pointer move 1142 662 sleep 500
		shot hint-ready.png
		pointer up sleep 700
		expect_log 'KWL OSK open kind=flick'
		;;
	large)
		compositor
		swipe 1912 1072 1770 930
		expect_log 'KWL OSK open kind=flick x=1506 y=44 width=414 height=1036'
		pointer move 900 400 sleep 400
		shot large-flick.png
		swipe 6 1072 150 930
		expect_log 'KWL OSK open kind=qwerty x=0 y=627 width=1920 height=453'
		pointer move 900 300 sleep 400
		shot large-qwerty.png
		;;
	qwerty)
		compositor
		guest 'rm -f /root/q.txt; touch /root/q.txt' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=600 /root/q.txt > /tmp/te.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		expect_log 'KWL MAP client='
		swipe 6 792 150 650
		expect_log 'KWL OSK open kind=qwerty'
		# The symbols face and back, so that both faces' keys are in the log.
		qkey_refresh
		qkey_tap '?123'
		expect_log 'KWL OSK qface name=symbols'
		pointer move 700 200 sleep 300
		shot qwerty-symbols.png
		qkey_refresh
		qkey_tap 'ABC'
		expect_log 'KWL OSK qface name=letters'
		# The 30 characters within 6 s.
		text='Hello, World! Kei 2026 (a+b)=c'
		guest "grep -a 'KWL OSK' /tmp/zdesktop.log" > "$out/qwerty-keys.txt"
		plan=$(python3 plan/ws102/tests/qwerty-plan.py "$out/qwerty-keys.txt" "$text" 4800) || { echo "qwerty-plan: FAILED"; status=1; }
		before=$(count 'KWL OSK qkey')
		pointer $plan
		sleep 1
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<ctrl-s>' >/dev/null
		sleep 2
		saved=$(read_file /root/q.txt)
		[ "$saved" = "$text" ] && echo "qwerty: typed \"$text\" ok" || { echo "qwerty: ($saved) MISSING"; status=1; }
		guest "grep -a 'KWL OSK qkey' /tmp/zdesktop.log" > "$out/qwerty-qkeys.txt"
		first=$(sed -n "$((before + 1))p" "$out/qwerty-qkeys.txt" | sed -n 's/.* ms=\([0-9]*\).*/\1/p')
		last=$(tail -1 "$out/qwerty-qkeys.txt" | sed -n 's/.* ms=\([0-9]*\).*/\1/p')
		elapsed=$(( ${last:-0} - ${first:-0} ))
		[ "$elapsed" -gt 0 ] && [ "$elapsed" -le 6000 ] && echo "qwerty: 30 characters in $elapsed ms (5 a second or faster) ok" || { echo "qwerty: $elapsed ms MISSING"; status=1; }
		pointer move 700 200 sleep 300
		shot qwerty.png
		# Shift twice locks it: A B C as capitals; once more unlocks it.
		qkey_refresh
		qkey_tap 'Shift' 2
		expect_log 'KWL OSK shift state=2'
		qkey_tap 'a'
		qkey_tap 'b'
		qkey_tap 'c'
		qkey_tap 'Shift'
		expect_log 'KWL OSK shift state=0'
		# The left arrow (key 105).
		qkey_tap '←'
		expect_log 'KWL OSK send via=key code=105 shift=0'
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<ctrl-s>' >/dev/null
		sleep 2
		saved=$(read_file /root/q.txt)
		[ "$saved" = "${text}ABC" ] && echo "qwerty: Shift locked: ABC ok" || { echo "qwerty: ($saved) MISSING"; status=1; }
		;;
	qwerty-ime)
		compositor
		expect_log 'KWL IME started pid='
		guest 'for p in $(ps -A -o pid,args | grep "[i]me-probe" | awk "{print \$1}"); do kill $p; done; rm -f /tmp/ime-probe.log' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/ime-probe --app-id=osk-ime --log=/tmp/ime-probe.log --seconds=300 > /dev/null 2>&1 </dev/null & sleep 4; echo started" >/dev/null
		expect_log 'KWL MAP client='
		# Japanese for the probe, then the QWERTY panel.
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<alt-spc>' >/dev/null
		sleep 1
		expect_log 'KWL IME language=ja'
		swipe 6 792 150 650
		expect_log 'KWL OSK open kind=qwerty'
		# a: the preedit あ.
		qkey_refresh
		qkey_tap 'a'
		expect_log 'KWL OSK send via=ime code=30'
		sleep 1
		guest 'cat /tmp/ime-probe.log' > "$out/ime-probe-qwerty.log"
		grep -qF 'preedit=あ' "$out/ime-probe-qwerty.log" && echo "qwerty-ime: a is the preedit あ ok" || { echo "qwerty-ime: preedit あ MISSING"; status=1; }
		# space converts; the candidates for the eye.
		qkey_tap 'space'
		expect_log 'KWL OSK send via=ime code=57'
		sleep 1
		pointer move 700 200 sleep 300
		shot qwerty-ime.png
		# Enter commits: a text, not the letter typed.
		qkey_tap 'Enter'
		expect_log 'KWL OSK send via=ime code=28'
		sleep 1
		guest 'cat /tmp/ime-probe.log' > "$out/ime-probe-qwerty.log"
		committed=$(grep -a 'PROBE TEXT text=' "$out/ime-probe-qwerty.log" | tail -1 | sed 's/.*PROBE TEXT text=//')
		if [ -n "$committed" ] && [ "$committed" != a ]; then
			echo "qwerty-ime: committed \"$committed\" ok"
		else
			echo "qwerty-ime: commit (\"$committed\") MISSING"
			status=1
		fi
		;;
	hand)
		compositor
		guest 'rm -f /tmp/ime-probe.log' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/ime-probe --log=/tmp/ime-probe.log --seconds=300 > /dev/null 2>&1 </dev/null & sleep 4; echo started" >/dev/null
		swipe 6 792 150 650
		expect_log 'KWL OSK open kind=qwerty x=0 y=464 width=1280 height=336'
		pointer move 1186 486 sleep 200 down sleep 60 up sleep 500
		expect_log 'KWL OSK hand on area=6,506,962,288'
		# Two strokes of the pointer, moving every 16 ms.
		pointer move 200 580 sleep 150 down sleep 16 move 220 590 sleep 16 move 240 600 sleep 16 move 260 612 sleep 16 move 280 625 sleep 16 \
			move 300 640 sleep 16 move 320 655 sleep 16 move 340 668 sleep 16 move 360 680 sleep 16 move 380 690 sleep 16 move 400 700 sleep 60 up sleep 150
		pointer move 420 560 sleep 150 down sleep 16 move 418 580 sleep 16 move 416 600 sleep 16 move 414 620 sleep 16 move 412 640 sleep 16 \
			move 410 660 sleep 16 move 408 680 sleep 16 move 406 700 sleep 16 move 404 720 sleep 16 move 402 740 sleep 60 up sleep 150
		# A stroke of a finger.
		touch_replay stroke 'size 1279 799 2
wait 2600
down 1 500 600
swipe 150 60 15 16
up 1
hold 1500'
		expect_log 'KWL OSK hand stroke-end strokes=3 '
		expect_text 'KWL OSK hand recognize strokes=3 '
		# The recognizer of ws165-p003 with the templates: some candidates and no note.
		expect_text 'KWL OSK hand templates path=/usr/share/keiland/hand/hershey.txt count=228 error=0'
		grep -E 'KWL OSK hand recognize strokes=3 .* candidates=[1-4] first=[^ ]+ note=$' "$out/osk-now.txt" >/dev/null || { echo "hand: no candidates"; status=1; }
		pointer move 700 300 sleep 300
		shot hand.png
		# Every frame that drew new points came within one frame of their input: its lag is shorter than the time since
		# the frame before (the guest's frame interval; 17 ms would need 60 frames a second, which this guest's
		# software Vulkan does not draw -- the numbers are kept for the record).
		guest "grep -a 'KWL OSK hand frame' /tmp/zdesktop.log" > "$out/hand-frames.txt"
		frames=$(grep -c 'lag_ms=' "$out/hand-frames.txt")
		late=$(sed -n 's/.*lag_ms=\([0-9]*\) gap_ms=\([0-9]*\).*/\1 \2/p' "$out/hand-frames.txt" | awk '$1 > $2 { n++ } END { print n + 0 }')
		worst=$(sed -n 's/.*lag_ms=\([0-9]*\).*/\1/p' "$out/hand-frames.txt" | sort -n | tail -1)
		gaps=$(sed -n 's/.*gap_ms=\([0-9]*\).*/\1/p' "$out/hand-frames.txt" | sort -n | awk '{ a[NR] = $1 } END { print a[int((NR + 1) / 2)] }')
		[ "${frames:-0}" -ge 5 ] && [ "${late:-1}" = 0 ] && echo "hand: $frames frames, each within one frame of its input (worst lag $worst ms, median frame interval $gaps ms) ok" || { echo "hand: $frames frames, $late late (worst lag $worst ms, median interval $gaps ms) MISSING"; status=1; }
		# The first candidate (1010,591), whatever the recognizer made of the strokes, to ime-probe; the ink is cleared.
		first=$(sed -n 's/.*KWL OSK hand recognize strokes=3 .* first=\([^ ]*\) note=$/\1/p' "$out/osk-now.txt" | tail -1)
		pointer move 1022 577 sleep 200 down sleep 60 up sleep 800
		guest 'cat /tmp/ime-probe.log' > "$out/ime-probe-hand.log"
		# A candidate a US key types (one ASCII byte: a Latin letter, a digit, a sign) arrives as that key, the others as
		# text (T1-310: "-" came as key 12).
		sent=
		if [ -n "$first" ] && [ "$(printf '%s' "$first" | wc -c)" -eq 1 ]; then
			grep -qE 'PROBE KEY key=[0-9]+ state=1' "$out/ime-probe-hand.log" && sent=key
		elif [ -n "$first" ]; then
			grep -qF "PROBE TEXT text=$first" "$out/ime-probe-hand.log" && sent=text
		fi
		[ -n "$sent" ] && echo "hand: the first candidate $first sent ($sent) ok" || { echo "hand: the first candidate ($first) MISSING"; status=1; }
		# A stroke, then clear (1035,668): no recognition follows.
		recognized=$(count 'KWL OSK hand recognize')
		pointer move 300 600 sleep 150 down sleep 16 move 330 610 sleep 16 move 360 620 sleep 60 up sleep 150
		pointer move 1047 665 sleep 200 down sleep 60 up sleep 1200
		expect_log 'KWL OSK hand clear'
		expect_count 'KWL OSK hand recognize' "$recognized"
		# Back to the keys.
		pointer move 1186 486 sleep 200 down sleep 60 up sleep 500
		expect_log 'KWL OSK hand off'
		;;
	extra)
		compositor
		guest 'rm -f /root/x.txt; touch /root/x.txt' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=600 /root/x.txt > /tmp/te.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		expect_log 'KWL MAP client='
		swipe 6 792 150 650
		expect_log 'KWL OSK open kind=qwerty'
		qkey_refresh
		qkey_tap 'b'
		qkey_tap 'c'
		qkey_tap 'Home'
		qkey_tap 'a'
		qkey_tap 'End'
		qkey_tap '|'
		qkey_tap 'Tab'
		expect_log 'KWL OSK send via=key code=102 shift=0 held=0'
		expect_log 'KWL OSK send via=key code=43 shift=1 held=0'
		expect_log 'KWL OSK send via=key code=15 shift=0 held=0'
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<ctrl-s>' >/dev/null
		sleep 2
		guest 'cat /root/x.txt' > "$out/x1.txt"
		printf 'abc|\t' > "$out/x1.want"
		cmp -s "$out/x1.txt" "$out/x1.want" && echo "extra: abc|<tab> ok" || { echo "extra: ($(od -c "$out/x1.txt" | head -2 | tr '\n' ' ')) MISSING"; status=1; }
		pointer move 700 200 sleep 300
		shot extra.png
		# Ctrl held for one key: Ctrl+A selects all; z replaces it; Ctrl is let go after the one key.
		qkey_tap 'Ctrl'
		expect_log 'KWL OSK held=4'
		qkey_tap 'a'
		expect_log 'KWL OSK send via=key code=30 shift=0 held=4'
		qkey_tap 'z'
		expect_log 'KWL OSK send via=key code=44 shift=0 held=0'
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<ctrl-s>' >/dev/null
		sleep 2
		saved=$(read_file /root/x.txt)
		[ "$saved" = "z" ] && echo "extra: Ctrl+A then z: z ok" || { echo "extra: ($saved) MISSING"; status=1; }
		# Alt twice lets go of it; Alt then Esc sends Esc with Alt once.
		qkey_tap 'Alt'
		qkey_tap 'Alt'
		expect_count 'KWL OSK held=0' 1
		qkey_tap 'Alt'
		qkey_tap 'Esc'
		expect_log 'KWL OSK send via=key code=1 shift=0 held=8'
		;;
	maxshot)
		# (a picture for the user, 2026-09-30) Text Editor docked (maximized) by a double click on its title bar, lines
		# typed on the QWERTY panel, the panel left open; the work area (p007) is not done, so the panel covers the
		# window's bottom.
		compositor
		guest 'rm -f /root/m.txt; touch /root/m.txt' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=600 /root/m.txt > /tmp/te.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		expect_log 'KWL MAP client='
		set -- $(guest "grep -a 'KWL MAP client=' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
		wx=${1:-0}; wy=${2:-0}
		pointer move $((wx + 200)) $((wy - 38)) sleep 200 down sleep 40 up sleep 120 down sleep 40 up sleep 1500
		expect_log 'KWL GLASS dock'
		swipe 6 $(( ${OSK_HEIGHT:-800} - 8 )) 150 $(( ${OSK_HEIGHT:-800} - 150 ))
		expect_log 'KWL OSK open kind=qwerty'
		qkey_refresh
		qkey_tap '?123'
		qkey_refresh
		qkey_tap 'ABC'
		qkey_refresh
		plan=$(python3 plan/ws102/tests/qwerty-plan.py "$out/qrect-now.txt" 'Hello from Kei!
The on-screen keyboard is part of the desktop.
Symbols: (a+b)*2 = c; x/y - 1 >= 0 ~ ok' 30000) || { echo "qwerty-plan: FAILED"; status=1; }
		pointer $plan
		sleep 1
		pointer move $(( ${OSK_WIDTH:-1280} / 2 )) 300 sleep 400
		shot "maximized-qwerty-${OSK_WIDTH:-1280}x${OSK_HEIGHT:-800}.png"
		;;
	maxflick)
		# (a picture for the user, 2026-09-30) Text Editor docked by a double click on its title bar; the flick panel
		# (the right column) types こんにちは, a new line, kei and 2026; the panel left open (the work area, p007, is not
		# done: the panel covers the window's right side).
		compositor
		guest 'rm -f /root/f.txt; touch /root/f.txt' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=600 /root/f.txt > /tmp/te.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		expect_log 'KWL MAP client='
		set -- $(guest "grep -a 'KWL MAP client=' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
		wx=${1:-0}; wy=${2:-0}
		pointer move $((wx + 200)) $((wy - 38)) sleep 200 down sleep 40 up sleep 120 down sleep 40 up sleep 1500
		expect_log 'KWL GLASS dock'
		width=${OSK_WIDTH:-1280}
		height=${OSK_HEIGHT:-800}
		swipe $((width - 8)) $((height - 8)) $((width - 150)) $((height - 150))
		expect_log 'KWL OSK open kind=flick'
		# The flick keys' middles: a key's side, the step between keys, the first column and row (the panel's bottom).
		fkey=$((height / 11)); [ $fkey -lt 64 ] && fkey=64; [ $fkey -gt 96 ] && fkey=96
		fstep=$((fkey + 6))
		fx=$((width - 4 * fkey - 30 + 6 + fkey / 2))
		fy=$((height - 4 * fstep + fkey / 2))
		fl=$((fkey / 2))
		# こ (か down), ん (わ up), に (な left), ち (た left), は (は), a new line.
		key_flick $((fx + fstep)) $fy 0 $fl
		key_flick $((fx + fstep)) $((fy + 3 * fstep)) 0 -$fl
		key_flick $((fx + fstep)) $((fy + fstep)) -$fl 0
		key_flick $fx $((fy + fstep)) -$fl 0
		key_tap $((fx + 2 * fstep)) $((fy + fstep))
		key_tap $((fx + 3 * fstep)) $((fy + 2 * fstep))
		# The alpha face: k (jkl left), e (def left), i (ghi up), a space.
		key_tap $((fx + 3 * fstep)) $((fy + 3 * fstep))
		key_flick $((fx + fstep)) $((fy + fstep)) -$fl 0
		key_flick $((fx + 2 * fstep)) $fy -$fl 0
		key_flick $fx $((fy + fstep)) 0 -$fl
		key_tap $((fx + 3 * fstep)) $((fy + fstep))
		# The number face: 2 0 2 6.
		key_tap $((fx + 3 * fstep)) $((fy + 3 * fstep))
		key_tap $((fx + fstep)) $fy
		key_tap $((fx + fstep)) $((fy + 3 * fstep))
		key_tap $((fx + fstep)) $fy
		key_tap $((fx + 2 * fstep)) $((fy + fstep))
		sleep 1
		pointer move $((width / 3)) 300 sleep 400
		shot "maximized-flick-${width}x${height}.png"
		;;
	workarea)
		# (p007) The work area: Text Editor docked, a floating wltest 500x400 over it; the QWERTY panel shortens the
		# docked window (told 1264x404 once: 1280 - 2 x 8 wide, 800 - the 44-pixel bar - the 336-pixel panel - 8 - 8 high, ws099-p031, p038) and moves the floating one up to the area's top (its bottom overhangs);
		# the flick panel narrows the docked window (962x752); closing gives the docked window its size back and moves
		# the floating one back; one moved by the user while the panel is out stays where it was put.
		compositor
		guest 'rm -f /root/w.txt; printf "The work area.\n" > /root/w.txt' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=600 /root/w.txt > /tmp/te.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		expect_log 'KWL MAP client='
		set -- $(guest "grep -a 'KWL MAP client=' /tmp/zdesktop.log | tail -1" | sed -n 's/.* surface=\([0-9]*\) x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2 \3/p')
		te=${1:-0}; wx=${2:-0}; wy=${3:-0}
		pointer move $((wx + 200)) $((wy - 38)) sleep 200 down sleep 40 up sleep 120 down sleep 40 up sleep 1500
		expect_log 'KWL GLASS dock'
		guest "export XDG_RUNTIME_DIR=/tmp; /bin/wltest --windowed --size=500x400 --frames=3600 > /dev/null 2>&1 </dev/null & sleep 4; echo started" >/dev/null
		set -- $(guest "grep -a 'KWL MAP client=' /tmp/zdesktop.log | tail -1" | sed -n 's/.* surface=\([0-9]*\) x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2 \3/p')
		wl=${1:-0}; lx=${2:-0}; ly=${3:-0}
		echo "docked surface $te, floating surface $wl at $lx,$ly"
		# The QWERTY panel: the docked window 1264x404, the floating one up to y=108 (the area's top under the 44-pixel bar).
		swipe 6 792 150 650
		expect_log 'KWL OSK work-area right=0 bottom=336'
		expect_log "KWL OSK work docked surface=$te width=1264 height=404"
		expect_log "KWL OSK work moved surface=$wl from=$lx,$ly to=$lx,108"
		sleep 1
		pointer move 640 200 sleep 300
		shot workarea-qwerty.png
		# Closed: the docked window whole again, the floating one back.
		pointer move 1254 488 sleep 200 down sleep 60 up sleep 900
		expect_log 'KWL OSK work-area right=0 bottom=0'
		expect_log "KWL OSK work docked surface=$te width=1264 height=740"
		expect_log "KWL OSK work back surface=$wl to=$lx,$ly"
		# The flick panel: the docked window 946x740 (the floating one fits already, or moves left).
		swipe 1272 792 1130 650
		expect_log 'KWL OSK work-area right=318 bottom=0'
		expect_log "KWL OSK work docked surface=$te width=946 height=740"
		sleep 1
		pointer move 400 200 sleep 300
		shot workarea-flick.png
		swipe 1272 792 1130 650
		expect_count 'KWL OSK work-area right=0 bottom=0' 2
		# The QWERTY panel again; the floating window moved by the user (its title bar dragged) stays where it was put.
		swipe 6 792 150 650
		expect_count "KWL OSK work moved surface=$wl " 2
		sleep 1
		pointer move $((lx + 150)) 76 sleep 200 down sleep 60 move $((lx + 120)) 86 sleep 60 move $((lx + 60)) 106 sleep 60 move $((lx + 40)) 116 sleep 150 up sleep 600
		expect_log "KWL GLASS moved surface=$wl "
		pointer move 1254 488 sleep 200 down sleep 60 up sleep 900
		expect_log "KWL OSK work kept surface=$wl"
		# (p016, p007's limit) The floating window dragged right (its right edge past the flick column); the flick panel
		# moves it left, the QWERTY panel opened in its place moves it from its first place (not the flick panel's), and
		# closing brings it back to that first place.
		set -- $(guest "grep -a 'KWL GLASS moved surface=$wl ' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
		mx=${1:-0}; my=${2:-0}
		pointer move $((mx + 150)) $((my - 22)) sleep 200 down sleep 60 move $((mx + 250)) $((my - 22)) sleep 60 move $((mx + 390)) $((my - 22)) sleep 150 up sleep 600
		set -- $(guest "grep -a 'KWL GLASS moved surface=$wl ' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
		hx=${1:-0}; hy=${2:-0}
		echo "floating window at $hx,$hy"
		swipe 1272 792 1130 650
		expect_log "KWL OSK work moved surface=$wl from=$hx,$hy to=450,$hy"
		sleep 1
		swipe 6 792 150 650
		expect_log "KWL OSK work moved surface=$wl from=450,$hy to=$hx,108"
		sleep 1
		pointer move 1254 488 sleep 200 down sleep 60 up sleep 900
		expect_log "KWL OSK work back surface=$wl to=$hx,$hy"
		;;
	roll)
		compositor
		guest 'rm -f /root/roll.txt; touch /root/roll.txt' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=600 /root/roll.txt > /tmp/te.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		expect_log 'KWL MAP client='
		swipe 6 792 150 650
		expect_log 'KWL OSK open kind=qwerty'
		qkey_refresh
		fplace=$(awk '$NF == "label=f"' "$out/qrect-now.txt" | tail -1 | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
		jplace=$(awk '$NF == "label=j"' "$out/qrect-now.txt" | tail -1 | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
		set -- $fplace $jplace
		if [ $# -ne 8 ]; then
			echo "roll: no places of f and j MISSING"
			status=1
		else
			fx=$(( $1 + $3 / 2 )) fy=$(( $2 + $4 / 2 )) jx=$(( $5 + $7 / 2 )) jy=$(( $6 + $8 / 2 ))
			# 100 taps, f and j in turn: each touch 50 ms before the other finger lifts, 50 ms more to the next touch.
			script="size 1279 799 2
wait 2600
down 1 $fx $fy
hold 50"
			i=1
			while [ $i -lt 100 ]; do
				if [ $((i % 2)) -eq 1 ]; then
					script="$script
down 2 $jx $jy
hold 50
up 1
hold 50"
				else
					script="$script
down 1 $fx $fy
hold 50
up 2
hold 50"
				fi
				i=$((i + 1))
			done
			script="$script
up 2
hold 800"
			before=$(count 'KWL OSK qkey')
			rolled=$(count 'KWL OSK touch down .* rollover=1')
			touch_replay roll "$script"
			sleep 1
			pointer move 700 200 sleep 300
			shot roll.png
			python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<ctrl-s>' >/dev/null
			sleep 2
			saved=$(guest 'cat /root/roll.txt' | tail -1)
			want=$(printf 'fj%.0s' $(seq 1 50))
			[ "$saved" = "$want" ] && echo "roll: 100 taps typed fj x50 ok" || { echo "roll: (${#saved} characters: $saved) MISSING"; status=1; }
			after=$(count 'KWL OSK qkey')
			[ $((after - before)) -eq 100 ] && echo "roll: 100 keys logged ok" || { echo "roll: $((after - before)) keys logged (expected 100) MISSING"; status=1; }
			rolls=$(( $(count 'KWL OSK touch down .* rollover=1') - rolled ))
			[ "$rolls" -eq 99 ] && echo "roll: 99 rollovers ok" || { echo "roll: $rolls rollovers (expected 99) MISSING"; status=1; }
		fi
		;;
	tools)
		# (p016) The flick panel's tools: two Text Editors (a.txt "hello world" on top, b.txt empty); three times: line
		# start, select, right x5, copy, the application before (b.txt), paste (and back to a.txt for the next time).
		# b.txt saved by Ctrl+S is "hellohellohello" (tools.png, tools-pasted.png).
		compositor
		guest 'printf "hello world\n" > /root/a.txt; rm -f /root/b.txt; touch /root/b.txt' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=900 /root/b.txt > /tmp/teb.log 2>&1 </dev/null & sleep 5; /bin/textedit --timeout-s=900 /root/a.txt > /tmp/tea.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		expect_count 'KWL MAP client=' 2
		swipe 1272 792 1130 650
		expect_log 'KWL OSK open kind=flick'
		sleep 1
		pointer move 400 300 sleep 300
		shot tools.png
		round=1
		while [ $round -le 3 ]; do
			tool_tap 1004 234
			tool_tap 1004 284
			n=0
			while [ $n -lt 5 ]; do tool_tap 1238 184; n=$((n + 1)); done
			tool_tap 1017 334
			tool_tap 1043 98
			tool_tap 1225 334
			[ $round -lt 3 ] && tool_tap 1043 98
			round=$((round + 1))
		done
		expect_count 'KWL OSK tool edit action=0 error=0' 3
		expect_count 'KWL OSK tool edit action=2 error=0' 3
		expect_count 'KWL OSK tool previous error=0' 5
		pointer move 400 300 sleep 300
		shot tools-pasted.png
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<ctrl-s>' >/dev/null
		sleep 2
		saved=$(read_file /root/b.txt)
		[ "$saved" = "hellohellohello" ] && echo "tools: hellohellohello pasted into b.txt ok" || { echo "tools: ($saved) MISSING"; status=1; }
		;;
	history)
		# (p024) The clipboard's history in the flick panel's history tab: three Text Editors copy "alpha", "bravo" and
		# "charlie" (Ctrl+A, Ctrl+C), a fourth (empty) takes the history's second row ("bravo") pasted
		# (history.png, history-pasted.png).
		compositor
		for word in alpha bravo charlie; do
			guest "printf '$word' > /root/$word.txt; export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=900 /root/$word.txt > /tmp/te-$word.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
			python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<ctrl-a>' '<ctrl-c>' >/dev/null
			sleep 1
		done
		guest "rm -f /root/receive.txt; touch /root/receive.txt; export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=900 /root/receive.txt > /tmp/te-receive.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		swipe 1272 792 1130 650
		expect_log 'KWL OSK open kind=flick'
		tool_tap 1160 141
		expect_log 'KWL OSK tool face=history items=3'
		sleep 1
		pointer move 400 300 sleep 300
		shot history.png
		tool_tap 1121 213
		expect_log 'KWL OSK history paste index=1 error=0'
		sleep 1
		pointer move 400 300 sleep 300
		shot history-pasted.png
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<ctrl-s>' >/dev/null
		sleep 2
		saved=$(read_file /root/receive.txt)
		[ "$saved" = "bravo" ] && echo "history: the second row (bravo) pasted ok" || { echo "history: ($saved) MISSING"; status=1; }
		;;
	emoji)
		# (p022) The emoji face: the places come from the log (KWL OSK etab / erect: x y width height), each tapped in
		# its middle.
		compositor
		guest 'rm -f /tmp/ime-probe.log' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/ime-probe --log=/tmp/ime-probe.log --seconds=300 > /dev/null 2>&1 </dev/null & sleep 4; echo started" >/dev/null
		swipe 1272 792 1130 650
		expect_log 'KWL OSK open kind=flick'
		tool_tap 1238 141
		expect_log 'KWL OSK tool face=emoji category=0'
		sleep 1
		pointer move 400 300 sleep 300
		shot emoji.png
		# The middle of a logged place: emoji_middle PATTERN sets ex ey.
		emoji_middle() {
			set -- $(guest "grep -a '$1' /tmp/zdesktop.log | tail -1" | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
			ex=$(( ${1:-0} + ${3:-0} / 2 ))
			ey=$(( ${2:-0} + ${4:-0} / 2 ))
		}
		# Category 0, index 0 (U+1F600) to ime-probe.
		emoji_middle 'KWL OSK erect category=0 index=0 '
		tool_tap "$ex" "$ey"
		expect_log 'KWL OSK emoji commit sent=1 '
		# Category 1's tab, then its index 3 (U+1F64C).
		emoji_middle 'KWL OSK etab category=1 '
		tool_tap "$ex" "$ey"
		expect_log 'KWL OSK emoji category=1'
		emoji_middle 'KWL OSK erect category=1 index=3 '
		tool_tap "$ex" "$ey"
		sleep 1
		guest 'cat /tmp/ime-probe.log' > "$out/ime-probe-emoji.log"
		grep -qF "PROBE TEXT text=$(printf '\360\237\230\200\360\237\231\214')" "$out/ime-probe-emoji.log" && echo "emoji: two emoji reached ime-probe ok" || { echo "emoji: ime-probe text MISSING"; status=1; }
		expect_count 'KWL OSK emoji commit sent=1 ' 2
		# wltest (no text input) on top: refused.
		guest 'for p in $(ps -A -o pid,args | grep "[i]me-probe" | awk "{print \$1}"); do kill $p; done' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp; /bin/wltest --windowed --frames=3600 > /dev/null 2>&1 </dev/null & sleep 4; echo started" >/dev/null
		emoji_middle 'KWL OSK erect category=1 index=0 '
		tool_tap "$ex" "$ey"
		expect_log 'KWL OSK emoji commit sent=0 '
		expect_log 'KWL OSK refused reason=no-text-input'
		guest 'for p in $(ps -A -o pid,args | grep "[w]ltest" | awk "{print \$1}"); do kill $p; done' >/dev/null
		# Text Editor (libkeiui's text input): category 1's index 0 (U+1F44D), saved; the file's bytes.
		guest 'rm -f /root/e.txt; touch /root/e.txt' >/dev/null
		guest "export XDG_RUNTIME_DIR=/tmp HOME=/root; /bin/textedit --timeout-s=600 /root/e.txt > /tmp/te-emoji.log 2>&1 </dev/null & sleep 5; echo started" >/dev/null
		tool_tap "$ex" "$ey"
		expect_count 'KWL OSK emoji commit sent=1 ' 3
		sleep 1
		pointer move 400 300 sleep 300
		shot emoji-sent.png
		python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<ctrl-s>' >/dev/null
		sleep 2
		bytes=$(guest 'od -An -tx1 /root/e.txt' | tr -d ' \n')
		case "$bytes" in
		f09f918d*) echo "emoji: Text Editor saved U+1F44D ok" ;;
		*) echo "emoji: Text Editor bytes ($bytes) MISSING"; status=1 ;;
		esac
		;;
	edges)
		compositor
		# A straight-up stroke from the bottom-right corner: no panel, no Wiseview.
		swipe 1272 792 1272 560
		expect_log 'KWL OSK cancel reason=(unarmed|direction)'
		expect_count 'KWL OSK open' 0
		expect_count 'KWL WISEVIEW open' 0
		# The bottom edge's swipe up in the middle opens Wiseview; a press in the middle closes it.
		swipe 640 796 640 480
		expect_log 'KWL WISEVIEW open windows='
		shot wiseview.png
		pointer move 640 400 sleep 200 down sleep 60 up sleep 900
		expect_count 'KWL OSK press' 1
		# A swipe right from the left edge above the corner switches the desktop.
		pointer move 6 700 sleep 200 down sleep 80 move 60 700 sleep 60 move 200 700 sleep 60 move 420 700 sleep 120 up sleep 1200
		expect_log 'KWL GLASS desktop swipe'
		expect_count 'KWL OSK press' 1
		;;
	touch)
		compositor
		# Ten swipes from the bottom-right corner: open, close, ... (the same swipe toggles).
		script='size 1279 799 2
wait 2600'
		i=0
		while [ $i -lt 10 ]; do
			script="$script
down 1 1272 792
swipe -150 -150 12 16
up 1
wait 400"
			i=$((i + 1))
		done
		touch_replay swipes "$script
hold 800"
		expect_count 'KWL OSK press corner=flick source=touch' 10
		expect_count 'KWL OSK commit corner=flick' 10
		expect_count 'KWL OSK open kind=flick' 5
		expect_count 'KWL OSK close kind=flick reason=gesture' 5
		# Ten straight-up strokes from the corner: nothing opens.
		script='size 1279 799 2
wait 2600'
		i=0
		while [ $i -lt 10 ]; do
			script="$script
down 1 1272 792
swipe 0 -200 12 16
up 1
wait 400"
			i=$((i + 1))
		done
		touch_replay straight "$script
hold 800"
		expect_count 'KWL OSK press corner=flick source=touch' 20
		expect_count 'KWL OSK open kind=flick' 5
		;;
	stop)
		guest "$stop_all" >/dev/null
		;;
	*)
		echo "unknown step $step"
		status=1
		;;
	esac
done
errors=$(count 'ERROR')
[ "$errors" = 0 ] && echo "zdesktop: no ERROR" || { echo "zdesktop: ERROR lines"; status=1; }
guest 'grep -a "KWL OSK" /tmp/zdesktop.log' > "$out/osk-log.txt"
[ $status = 0 ] && echo "osk-guest: PASS" || echo "osk-guest: FAIL"
exit $status
