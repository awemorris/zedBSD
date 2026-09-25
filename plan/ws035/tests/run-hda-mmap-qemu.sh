#!/bin/sh
# ws035-p049: the /dev/dsp mmap interface on QEMU's HD Audio, next to the
# write path it must not disturb.  Each guest transcript and WAV check is
# kept under OUT.  IMG is the image built with config-amd64-hda-image.mk.
set -u
cd /home/awe/zedBSD-rpi4
OUT=${OUT:-plan/ws035/phase049/evidence}
D=${RUN:-/tmp/p049-run}; export RUN=$D
export IMG=${IMG:-build/p049-img/hdd-image.img}
mkdir -p "$OUT"
WAV="-audiodev wav,id=w,path=$D/out.wav,out.frequency=48000,out.channels=2,out.format=s16"
G="python3 plan/tools/guest/serial.py --socket $D/serial.sock --timeout 120"
Q=plan/tools/qmp.py
C=plan/ws035/tests/hda-wav-check.py

run() {
	name=$1; devices=$2; commands=$3; check=$4
	echo "=== $name" | tee "$OUT/$name.txt"
	QEMU_EXTRA="$devices" sh plan/ws035/tests/boot-hda.sh >> "$OUT/$name.txt" 2>&1
	$G run "$commands" > "$D/guest.txt" 2>&1
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

run ich9-mmap-bitexact \
    "-device ich9-intel-hda,id=hda -device hda-output,bus=hda.0,audiodev=w,mixer=off $WAV" \
    "audiotest mmapplay 96000" "pattern $D/out.wav 96000"
run ich9-write-still-bitexact \
    "-device ich9-intel-hda,id=hda -device hda-output,bus=hda.0,audiodev=w,mixer=off $WAV" \
    "audiotest play 96000" "pattern $D/out.wav 96000"
run ich9-mmap-then-write \
    "-device ich9-intel-hda,id=hda -device hda-output,bus=hda.0,audiodev=w,mixer=off $WAV" \
    "audiotest mmapplay 24000; audiotest play 48000" "patterns $D/out.wav 24000 48000"
run ich9-duplex-mmap-capture \
    "-device ich9-intel-hda,id=hda -device hda-duplex,bus=hda.0,audiodev=n -audiodev none,id=n" \
    "audiotest mmapcapture 48000; audiotest mmapcapture 144000; audiotest capture 48000" ""
