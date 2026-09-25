#!/bin/sh
# ws035-p009: audiod on QEMU's HD Audio, driven by audiod-client.  Each guest
# transcript and WAV check is kept under OUT.
set -u
cd /home/awe/zedBSD-rpi4
OUT=${OUT:-plan/ws035/phase009/evidence}
D=${RUN:-/tmp/p009-run}; export RUN=$D
export IMG=${IMG:-build/p009-img/hdd-image.img}
mkdir -p "$OUT"
WAV="-audiodev wav,id=w,path=$D/out.wav,out.frequency=48000,out.channels=2,out.format=s16"
G="python3 plan/tools/guest/serial.py --socket $D/serial.sock --timeout 120"
Q=plan/tools/qmp.py
C=plan/ws035/tests/hda-wav-check.py

run() {
	name=$1; devices=$2; commands=$3; check=$4
	echo "=== $name" | tee "$OUT/$name.txt"
	QEMU_EXTRA="$devices" sh plan/ws035/tests/boot-hda.sh >> "$OUT/$name.txt" 2>&1
	$G run "service status audiod; $commands" > "$D/guest.txt" 2>&1
	status=$?
	tr -d '\r' < "$D/guest.txt" >> "$OUT/$name.txt"
	echo "guest-status=$status" >> "$OUT/$name.txt"
	python3 $Q $D/qmp.sock quit > /dev/null 2>&1; sleep 2
	if [ -n "$check" ]; then
		python3 $C $check >> "$OUT/$name.txt" 2>&1
		echo "check-status=$?" >> "$OUT/$name.txt"
	fi
	cat "$OUT/$name.txt"
}

OUTPUT="-device ich9-intel-hda,id=hda -device hda-output,bus=hda.0,audiodev=w,mixer=off $WAV"
run audiod-bitexact "$OUTPUT" "audiod-client play 96000" "pattern $D/out.wav 96000"
run audiod-sum "$OUTPUT" "audiod-client sum 48000 3000 5000" "windows $D/out.wav"
run audiod-volume "$OUTPUT" "audiod-client constant 48000 16000 32768" "windows $D/out.wav"
run audiod-resample "$OUTPUT" "audiod-client play 44100 44100 1" "windows $D/out.wav"
run audiod-capture \
    "-device ich9-intel-hda,id=hda -device hda-duplex,bus=hda.0,audiodev=n -audiodev none,id=n" \
    "audiod-client capture 48000; audiod-client capture 144000" ""
run audiod-shrink "$OUTPUT" "audiod-client shrink 48000; service status audiod" "pattern $D/out.wav 48000"
run audiod-no-device "" "audiod-client play 48000; audiod-client capture 48000" ""
