#!/bin/sh
# ws100-p005: Settings' Sound page sets the system bar's volume, and follows it, on the Venus guest of the volume image
# (build-volume-image.sh) with QEMU's HD Audio (volume-guest.sh), kei's session at boot.  Settings runs in kei's
# session with kei's preferences (HOME=/home/kei).
#  1. The page: audiod reached with a device (SOUND report reachable=1 device=1); sound.png.
#  2. Settings -> audiod and the system bar: the slider dragged to 30% sets audiod to 30, with one feedback sound at
#     the release, and leaves desktop.conf as it was (BUG-161, ws100-p012: audiod holds the volume during the
#     session; zdesktop writes it once at the session's end, volume-p004's A5); the system bar's popup shows it
#     (bar-30.png).
#  3. Mute in Settings: audiod muted, no feedback sound; the bar's icon muted (bar-muted.png); mute off: a sound.
#  4. The system bar -> Settings: the wheel over the bar's icon (two notches down) and the bar's mute are shown by the
#     page within a few seconds (SOUND report value=20, muted=1) (page-20.png, page-muted.png).
#  5. The WAV (the guest stopped): the feedback sounds were played (at least the releases' and mute-off's).
#   plan/ws100/tests/volume-p005.sh IMAGE [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
image=${1:?usage: volume-p005.sh IMAGE [OUTDIR]}
out=${2:-build/ws100-shots/p005}
mkdir -p "$out"
GUEST_RUNTIME=$(pwd)/build/ws100-run
export GUEST_RUNTIME
log=/run/user/1000/session.log
slog=/tmp/s.log
conf=/home/kei/.config/keiland/desktop.conf
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py "$GUEST_RUNTIME/qmp.sock" "$@"; }
shot() { pointer move 1270 790 sleep 600; python3 plan/ws035/tests/zdesktop-check.py "$out/$1" --runtime "$GUEST_RUNTIME" >/dev/null 2>&1; echo "shot: $out/$1"; }
status=0

# Records a verdict.
verdict() {
	if [ "$1" = ok ]; then
		echo "$2 ok"
	else
		echo "$2 FAIL"
		status=1
	fi
}

# The number of lines of a guest file matching a pattern.
count() {
	guest "grep -cE '$2' $1" | tail -1
}

# Waits until a guest file has more than N lines matching a pattern (within some seconds).
expect_more() {
	tries=0
	found=0
	while [ $tries -lt "$4" ]; do
		found=$(count "$1" "$2")
		[ "${found:-0}" -gt "$3" ] 2>/dev/null && break
		tries=$((tries + 1))
		sleep 1
	done
	if [ "${found:-0}" -gt "$3" ] 2>/dev/null; then
		echo "log: $2 ok"
		return 0
	fi
	echo "log: $2 MISSING"
	status=1
	return 1
}

# audiod's volume as "left muted".
audiod_volume() {
	guest 'audiod-feedback get' | sed -n 's/.*volume left=\([0-9]*\) right=[0-9]* muted=\([0-9]\).*/\1 \2/p' | tail -1
}

# The last line of a guest file matching a pattern.
last() {
	guest "grep -E '$2' $1 | tail -1"
}

# Finds a Settings control's rectangle in window coordinates (the last one logged): sets cx0 cy0 cw ch.
find_control() {
	set -- $(last $slog "ZSETTINGS CONTROL index=$1 " | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
	cx0=${1:-0}; cy0=${2:-0}; cw=${3:-0}; ch=${4:-0}
}

# Drags the volume's slider from the middle to a percentage.
slide_to() {
	find_control 6
	cy=$((wy + cy0 + ch / 2)); start=$((wx + cx0 + 12 + (cw - 24) / 2))
	end=$((wx + cx0 + 12 + (cw - 24) * $1 / 100))
	pointer move "$start" "$cy" sleep 200 down sleep 100 move $(((start + end) / 2)) "$cy" sleep 150 move "$end" "$cy" sleep 300 up sleep 1500
}

# Clicks the mute switch.
mute_click() {
	find_control 7
	cx=$((wx + cx0 + cw / 2)); cy=$((wy + cy0 + ch / 2))
	pointer move $((cx - 2)) "$cy" sleep 150 move "$cx" "$cy" sleep 300 down sleep 60 up sleep 1500
}

# Opens the system bar's popup (the icon's place from the log), takes a picture, and closes it with Esc.
bar_shot() {
	name=$1
	set -- $(last $log 'KWL VOLUME icon x=' | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
	ix=$((${1:-900} + ${3:-30} / 2)); iy=$((${2:-3} + ${4:-28} / 2))
	pointer move $((ix - 2)) $iy sleep 200 move $ix $iy sleep 300 down sleep 60 up sleep 900
	python3 plan/ws035/tests/zdesktop-check.py "$out/$name" --runtime "$GUEST_RUNTIME" >/dev/null 2>&1
	echo "shot: $out/$name"
	python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<esc>'
	sleep 0.8
}

# 0. The guest with HD Audio recording, kei's session, and Settings on the Sound page in it.
sh plan/ws100/tests/volume-guest.sh stop >/dev/null 2>&1
VOLUME_AUDIO=duplex timeout 180 sh plan/ws100/tests/volume-guest.sh start "$image" >/dev/null 2>&1
sleep 35
expect_more $log 'KWL HANDOFF go=1' 0 60
expect_more $log 'KWL VOLUME reachable=1 device=1' 0 20
guest 'audiod-feedback volume 60' >/dev/null
sleep 2
conf_start=$(guest "grep -E '^sound\\.(volume|muted)=' $conf" | tr '\n' ' ')

# A Settings already running (the Welcome the session starts at a first login, ws164-p002) would take the page this
# test asks for, with its lines in the session's log and its window mapped before (T1-478: the five lines that read
# Settings' log failed): it ends first.  What ran is kept (processes.txt).
guest 'ps -A -o pid,args' > "$out/processes.txt"
guest 'for p in $(ps -A -o pid,args | awk '"'"'{n = $2; sub(/.*\//, "", n)} n == "settings" {print $1}'"'"'); do kill $p; done; sleep 2; echo ended' >/dev/null
maps=$(count $log 'KWL MAP client=')
# Settings runs as kei (runas): the compositor serves its system extension only to its own user (WS131 p011, D5), so a
# Settings root started would find no sound (T2-021).
guest "export XDG_RUNTIME_DIR=/run/user/1000 HOME=/home/kei; /bin/runas kei /bin/settings --timeout-s=600 sound > $slog 2>&1 </dev/null & sleep 6; echo started" >/dev/null
expect_more $log 'KWL MAP client=' "$maps" 20
set -- $(last $log 'KWL MAP client=' | sed -n 's/.* x=\([-0-9]*\) y=\([-0-9]*\).*/\1 \2/p')
wx=${1:-0}; wy=${2:-0}
echo "settings: window at $wx,$wy"

# 1. The page reaches audiod.
expect_more $slog 'SOUND report reachable=1 device=1 value=60' 0 10
expect_more $slog 'ZSETTINGS CONTROL index=6 ' 0 10
shot sound.png

# 2. Settings -> audiod and the bar, not desktop.conf.
feedbacks=$(count $slog 'SOUND feedback error=0')
slide_to 30
expect_more $slog 'SOUND set value=(29|30|31) muted=0 final=1' 0 5
set -- $(audiod_volume)
[ "${1:-0}" -ge 29 ] && [ "${1:-0}" -le 31 ] && [ "${2:-1}" = 0 ] && verdict ok "settings slider: audiod at ${1:-?}" || verdict no "settings slider: audiod at ${1:-?} muted ${2:-?}"
value=${1:-30}
sleep 1
kept=$(guest "grep -E '^sound\\.(volume|muted)=' $conf" | tr '\n' ' ')
echo "desktop.conf: $kept (at the start: $conf_start)"
[ "$kept" = "$conf_start" ] && verdict ok "settings slider: desktop.conf not written" || verdict no "settings slider: desktop.conf not written"
after=$(count $slog 'SOUND feedback error=0')
[ "${after:-0}" -gt "${feedbacks:-0}" ] && verdict ok "settings slider: feedback sound at the release" || verdict no "settings slider: feedback sound at the release"
bar_shot bar-30.png

# 3. Mute in Settings (no sound), and off again (a sound).
feedbacks=$(count $slog 'SOUND feedback error=0')
mute_click
set -- $(audiod_volume)
[ "${2:-0}" = 1 ] && verdict ok "settings mute on: audiod muted" || verdict no "settings mute on: audiod muted (${2:-?})"
after=$(count $slog 'SOUND feedback error=0')
[ "${after:-0}" = "${feedbacks:-x}" ] && verdict ok "settings mute on: no feedback sound" || verdict no "settings mute on: no feedback sound"
shot page-muted-here.png
bar_shot bar-muted.png
mute_click
set -- $(audiod_volume)
[ "${2:-1}" = 0 ] && verdict ok "settings mute off: audiod unmuted" || verdict no "settings mute off: audiod unmuted (${2:-?})"
after2=$(count $slog 'SOUND feedback error=0')
[ "${after2:-0}" -gt "${after:-0}" ] && verdict ok "settings mute off: feedback sound" || verdict no "settings mute off: feedback sound"

# 4. The bar -> Settings: two notches down on the bar's icon, then the bar's mute row.
set -- $(last $log 'KWL VOLUME icon x=' | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p')
ix=$((${1:-900} + ${3:-30} / 2)); iy=$((${2:-3} + ${4:-28} / 2))
pointer move $ix $iy sleep 400 wheel-down sleep 700 wheel-down sleep 1000
target=$((value - 10))
expect_more $slog "SOUND report reachable=1 device=1 value=$target muted=0" 0 5
shot page-$target.png
pointer move $((ix - 2)) $iy sleep 200 move $ix $iy sleep 300 down sleep 60 up sleep 900
set -- $(last $log 'KWL VOLUME popup open' | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\) width=\([0-9]*\) height=\([0-9]*\) slider=\([0-9]*\) mute=\([0-9]*\).*/\1 \2 \3 \4 \5 \6/p')
px=${1:-900} pw=${3:-260} my=$((${6:-134} + 17))
pointer move $((px + pw - 40)) $my sleep 300 down sleep 60 up sleep 800
python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" '<esc>'
expect_more $slog "SOUND report reachable=1 device=1 value=$target muted=1" 0 5
shot page-muted.png

# The logs, and no errors.
guest "grep -E 'ZSETTINGS (SOUND|LOOK set key=sound)' $slog" > "$out/settings-sound.log"
guest "grep -E 'KWL (VOLUME|ERROR)' $log" > "$out/session-volume.log"
grep -q 'KWL ERROR' "$out/session-volume.log" && { echo "KWL ERROR in the session"; status=1; }
alive=$(guest "ps -A -o args | grep -c '[s]ettings'" | tail -1)
[ "${alive:-0}" -ge 1 ] && verdict ok "settings runs" || verdict no "settings runs"

# 5. The WAV, once the guest is stopped.
sh plan/ws100/tests/volume-guest.sh stop >/dev/null 2>&1
sleep 2
cp "$GUEST_RUNTIME/out.wav" "$out/p005.wav" 2>/dev/null
python3 plan/ws035/tests/hda-wav-check.py windows "$out/p005.wav" > "$out/windows.txt" 2>&1
sounds=$(python3 - "$out/windows.txt" <<'EOF'
import re, sys
line = [l for l in open(sys.argv[1]).read().splitlines() if l.startswith("windows:")]
values = [int(v) for v in re.findall(r"-?\d+", line[0])] if line else []
runs, current = [], None
for index, value in enumerate(values):
	if value > 0:
		if current is None:
			current = [index, value]
			runs.append(current)
		else:
			current[1] = max(current[1], value)
	else:
		current = None
print(" ".join("%d:%d" % (r[0], r[1]) for r in runs))
EOF
)
echo "sounds (quarter second: peak): $sounds"
n=$(echo "$sounds" | wc -w)
[ "$n" -ge 3 ] && verdict ok "feedback sounds recorded ($n)" || verdict no "feedback sounds recorded ($n)"
[ $status -eq 0 ] && echo "volume-p005: PASS" || echo "volume-p005: FAIL"
exit $status
