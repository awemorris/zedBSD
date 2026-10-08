#!/bin/sh
# ws100-p002: audiod's feedback sound and its own volume on QEMU's HD Audio, recorded to WAV.  Each case boots the audiod
# test image (build-audiod-image.sh) with its own QEMU in RUN (only that QEMU is started and stopped), logs in on the
# serial console, runs audiod-feedback, stops QEMU and checks the WAV (plan/ws035/tests/hda-wav-check.py windows: the
# largest |sample| of each quarter second).
#   feedback      hda-duplex, mixer on (the codec's volume): FEEDBACK three times 1 s apart -> three windows with sound
#   restart       FEEDBACK five times 30 ms apart: one sound (each starts it again), no window louder than one sound
#   soft          hda-duplex, mixer=off (QEMU leaves the codec's volume out; audiod's own is checked by the kernel's
#                 answer): volume 100, 50, 10, 0 then FEEDBACK each -> the peaks fall
#   mute          mute, FEEDBACK -> silence; volume 100, FEEDBACK -> sound
#   hardware      hda-duplex, mixer on: volume 100, 50, 10 then FEEDBACK each: the codec's amplifier (QEMU applies it to
#                 the WAV) lowers the sound; the peaks are reported (how pci-hda maps percent to the codec's steps)
#   curve         ws100-p009: mixer=off, volume 100, 75, 50, 25 then FEEDBACK each: the peaks are -6, -15, -30 dB below
#                 the first +- 3 dB (plan/ws100/tests/volume-curve.py wav); mixer on, the same volumes: the steps pci-hda
#                 chose (dmesg's "hda: volume" lines) are the curve's (volume-curve.py dmesg)
#   no-device     no HD Audio: FEEDBACK is answered (DONE) and nothing breaks
#   unknown       raw 99 -> ERROR EINVAL (3 in zedBSD)
#   RUN=build/ws100-run IMG=build/ws100-audiod.img plan/ws100/tests/audiod-qemu.sh [OUT] [CASE...]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
# KVM when the host offers it, else TCG; QEMU_NO_KVM=1 forces TCG (ws129-p011).
. "$(dirname -- "$0")/../../tools/guest/qemu-accel.sh"
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws100-audiod}
[ $# -ge 1 ] && shift
cases=${*:-"feedback restart soft mute hardware curve no-device unknown"}
D=${RUN:-$(pwd)/build/ws100-run}
IMG=${IMG:-build/ws100-audiod.img}
mkdir -p "$out" "$D"
G="python3 plan/tools/guest/serial.py --socket $D/serial.sock --timeout 120"
C=plan/ws035/tests/hda-wav-check.py
WAV="-audiodev wav,id=w,path=$D/out.wav,out.frequency=48000,out.channels=2,out.format=s16"
status=0

# Stops this run's QEMU only (its pid file).
stop_qemu() {
	if [ -f "$D/qemu.pid" ]; then
		python3 - "$D/qmp.sock" <<'EOF' >/dev/null 2>&1
import json, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(5); s.connect(sys.argv[1])
f = s.makefile("rw"); f.readline()
for c in ({"execute": "qmp_capabilities"}, {"execute": "quit"}):
	f.write(json.dumps(c) + "\n"); f.flush()
	try:
		f.readline()
	except OSError:
		pass
EOF
		sleep 2
		kill "$(cat "$D/qemu.pid")" 2>/dev/null
		rm -f "$D/qemu.pid"
	fi
}

# Boots the image with some audio devices and logs in on the serial console.
boot() {
	stop_qemu
	rm -f "$D"/*.sock "$D"/out.wav
	cp "$IMG" "$D/stick.img"
	cp /usr/share/OVMF/OVMF_VARS_4M.fd "$D/vars.fd"
	qemu-system-x86_64 -machine q35 -m 512 -smp 4 $(qemu_accel_args max) \
	    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	    -drive if=pflash,format=raw,file="$D/vars.fd" -device qemu-xhci,id=xhci \
	    -drive if=none,id=boot,file="$D/stick.img",format=raw \
	    -device usb-storage,bus=xhci.0,port=1,drive=boot,id=rootstick,bootindex=1 \
	    -serial unix:"$D/serial.sock",server,nowait -vga std -display none -no-reboot \
	    -qmp unix:"$D/qmp.sock",server,nowait $1 > "$D/qemu.log" 2>&1 &
	echo $! > "$D/qemu.pid"
	sleep 5
	for attempt in 1 2 3 4 5 6 7 8 9 10 11 12; do
		timeout 60 $G login >/dev/null 2>&1 && return 0
	done
	echo "serial: login failed"
	return 1
}

# One case: boots, runs the client's words (and an extra command after them), stops QEMU, and keeps the transcript.
run() {
	name=$1 devices=$2 words=$3 extra=${4:-true}
	echo "=== $name" > "$out/$name.txt"
	boot "$devices" >> "$out/$name.txt" 2>&1 || { status=1; return; }
	timeout 150 $G run "service status audiod; audiod-feedback $words; echo client-status=\$?; $extra" > "$D/guest.txt" 2>&1
	tr -d '\r' < "$D/guest.txt" >> "$out/$name.txt"
	stop_qemu
	[ -f "$D/out.wav" ] && cp "$D/out.wav" "$out/$name.wav"
}

# Prints the windows (quarter seconds) of a WAV with sound above a level, as "index:peak".
loud() {
	python3 "$C" windows "$1" 2>/dev/null | python3 -c '
import re, sys
level = int(sys.argv[1])
line = [l for l in sys.stdin.read().splitlines() if l.startswith("windows:")]
values = [int(v) for v in re.findall(r"-?\d+", line[0])] if line else []
print(" ".join("%d:%d" % (i, v) for i, v in enumerate(values) if v > level))' "$2"
}

# Records a verdict.
verdict() {
	if [ "$1" = ok ]; then
		echo "$2 ok"
	else
		echo "$2 FAIL"
		status=1
	fi
}

DUPLEX="-device intel-hda,id=hda -device hda-duplex,bus=hda.0,audiodev=w $WAV"
DUPLEX_OFF="-device intel-hda,id=hda -device hda-duplex,bus=hda.0,audiodev=w,mixer=off $WAV"
for case in $cases; do
	case $case in
	feedback)
		run feedback "$DUPLEX" "volume 100 sleep 500 feedback sleep 1000 feedback sleep 1000 feedback sleep 1000"
		windows=$(loud "$out/feedback.wav" 500)
		echo "feedback: windows with sound: $windows"
		n=$(echo "$windows" | wc -w)
		[ "$n" -ge 3 ] && [ "$n" -le 6 ] && verdict ok "feedback: three sounds ($n windows)" || verdict no "feedback: three sounds ($n windows)"
		grep -q 'client-status=0' "$out/feedback.txt" && verdict ok "feedback: answered" || verdict no "feedback: answered"
		;;
	restart)
		run restart "$DUPLEX" "volume 100 sleep 500 feedback sleep 1000 feedback sleep 30 feedback sleep 30 feedback sleep 30 feedback sleep 30 feedback sleep 1000"
		python3 "$C" windows "$out/restart.wav" > "$out/restart-windows.txt" 2>&1
		windows=$(loud "$out/restart.wav" 500)
		echo "restart: windows with sound: $windows"
		# The single sound's peak (the first) against the quick run's: not louder (no overlap adds up).
		first=$(echo "$windows" | tr ' ' '\n' | head -1 | cut -d: -f2)
		most=$(echo "$windows" | tr ' ' '\n' | cut -d: -f2 | sort -n | tail -1)
		[ -n "$first" ] && [ "${most:-0}" -le $((first * 11 / 10)) ] && verdict ok "restart: no overlap (single $first, largest $most)" || verdict no "restart: no overlap (single ${first:-?}, largest ${most:-?})"
		;;
	soft)
		run soft "$DUPLEX_OFF" "volume 100 sleep 500 feedback sleep 1000 volume 50 feedback sleep 1000 volume 10 feedback sleep 1000 volume 0 feedback sleep 1000 get"
		windows=$(loud "$out/soft.wav" 0)
		echo "soft: windows with sound: $windows"
		# Four sounds in order: 100 percent, 50 (-30 dB), 10 (-54 dB), 0 (silent): the peaks fall.
		set -- $(echo "$windows" | tr ' ' '\n' | awk -F: 'NR == 1 || $1 > last + 1 { print $2 } { last = $1 }')
		echo "soft: peaks of the sounds: $*"
		[ $# -eq 3 ] && [ "$1" -gt "$2" ] && [ "$2" -gt "$3" ] && [ "$3" -gt 0 ] && verdict ok "soft: 100 > 50 > 10 > 0 (silent)" || verdict no "soft: 100 > 50 > 10 > 0 (silent)"
		python3 "$C" windows "$out/soft.wav" > "$out/soft-windows.txt" 2>&1
		grep -q 'volume left=0 ' "$out/soft.txt" && verdict ok "soft: audiod reports 0" || verdict no "soft: audiod reports 0"
		;;
	mute)
		run mute "$DUPLEX" "mute sleep 500 feedback sleep 1000 volume 100 feedback sleep 1000 get"
		windows=$(loud "$out/mute.wav" 0)
		echo "mute: windows with sound: $windows"
		# Only the unmuted sound: one run of windows.
		n=$(echo "$windows" | tr ' ' '\n' | awk -F: 'NR == 1 || $1 > last + 1 { n++ } { last = $1 } END { print n + 0 }')
		[ "$n" -eq 1 ] && verdict ok "mute: silent while muted, heard after" || verdict no "mute: silent while muted, heard after ($n sounds)"
		;;
	hardware)
		run hardware "$DUPLEX" "volume 100 sleep 500 feedback sleep 1000 volume 50 feedback sleep 1000 volume 10 feedback sleep 1000 get"
		python3 "$C" windows "$out/hardware.wav" > "$out/hardware-windows.txt" 2>&1
		peaks=$(loud "$out/hardware.wav" 0)
		echo "hardware: windows with sound: $peaks"
		n=$(echo "$peaks" | wc -w)
		[ "$n" -ge 2 ] && verdict ok "hardware: the sounds are recorded" || verdict no "hardware: the sounds are recorded"
		;;
	curve)
		# ws100-p009: audiod's own volume (mixer=off) at 100, 75, 50 and 25 %: the peaks' dB against the curve.
		run curve-soft "$DUPLEX_OFF" "volume 100 sleep 500 feedback sleep 1000 volume 75 feedback sleep 1000 volume 50 feedback sleep 1000 volume 25 feedback sleep 1000 get"
		python3 "$C" windows "$out/curve-soft.wav" > "$out/curve-soft-windows.txt" 2>&1
		python3 plan/ws100/tests/volume-curve.py wav "$out/curve-soft-windows.txt" > "$out/curve-soft-db.txt" 2>&1
		sed 's/^/curve-soft: /' "$out/curve-soft-db.txt"
		grep -q 'volume-curve: PASS' "$out/curve-soft-db.txt" && verdict ok "curve: audiod's volume on the curve" || verdict no "curve: audiod's volume on the curve"
		# The codec's amplifier (mixer on): the steps pci-hda chose (its lines, read with dmesg on the guest's shell).
		run curve-codec "$DUPLEX" "volume 100 sleep 300 volume 75 sleep 300 volume 50 sleep 300 volume 25 sleep 300 get" "dmesg | grep 'hda: volume'"
		python3 plan/ws100/tests/volume-curve.py dmesg "$out/curve-codec.txt" > "$out/curve-codec-db.txt" 2>&1
		sed 's/^/curve-codec: /' "$out/curve-codec-db.txt"
		grep -q 'volume-curve: PASS' "$out/curve-codec-db.txt" && verdict ok "curve: the codec's steps on the curve" || verdict no "curve: the codec's steps on the curve"
		;;
	no-device)
		run no-device "" "feedback sleep 300 feedback get"
		grep -q 'welcome device=0' "$out/no-device.txt" && verdict ok "no-device: device=0" || verdict no "no-device: device=0"
		grep -q 'client-status=0' "$out/no-device.txt" && verdict ok "no-device: FEEDBACK answered" || verdict no "no-device: FEEDBACK answered"
		;;
	unknown)
		run unknown "$DUPLEX" "raw 99"
		# zedBSD's EINVAL is 3 (include/uapi/errno.h).
		grep -q 'done type=67 error=3$' "$out/unknown.txt" && verdict ok "unknown: ERROR EINVAL" || verdict no "unknown: ERROR EINVAL"
		;;
	esac
done
stop_qemu
[ $status -eq 0 ] && echo "audiod-qemu: PASS" || echo "audiod-qemu: FAIL"
exit $status
