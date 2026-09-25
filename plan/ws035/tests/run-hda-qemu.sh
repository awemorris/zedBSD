#!/bin/sh
# ws035-p007: runs the HDA driver through five QEMU configurations and
# keeps each guest transcript and WAV check under OUT.
set -u
cd /home/awe/zedBSD-rpi4
OUT=${OUT:-plan/ws035/phase007/evidence}
D=${RUN:-/tmp/p007-run}; export RUN=$D
mkdir -p "$OUT"
WAV="-audiodev wav,id=w,path=$D/out.wav,out.frequency=48000,out.channels=2,out.format=s16"
G="python3 plan/tools/guest/serial.py --socket $D/serial.sock --timeout 120"
Q=plan/tools/qmp.py
C=plan/ws035/tests/hda-wav-check.py

run() {
	name=$1; devices=$2; commands=$3; check=$4
	echo "=== $name" | tee "$OUT/$name.txt"
	QEMU_EXTRA="$devices" sh plan/ws035/tests/boot-hda.sh >> "$OUT/$name.txt" 2>&1
	$G run "dmesg | grep hda; $commands" > "$D/guest.txt" 2>&1
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

run ich9-output-bitexact \
    "-device ich9-intel-hda,id=hda -device hda-output,bus=hda.0,audiodev=w,mixer=off $WAV" \
    "audiotest info; audiotest play 96000" "pattern $D/out.wav 96000"
run ich9-output-short \
    "-device ich9-intel-hda,id=hda -device hda-output,bus=hda.0,audiodev=w,mixer=off $WAV" \
    "audiotest play 1000" "pattern $D/out.wav 1000"
run ich9-output-volume \
    "-device ich9-intel-hda,id=hda -device hda-output,bus=hda.0,audiodev=w $WAV" \
    "audiotest tone 48000 16000; audiotest volume 50 50 0; audiotest tone 48000 16000; audiotest volume 50 50 1; audiotest tone 48000 16000" \
    "windows $D/out.wav"
run ich9-duplex-capture \
    "-device ich9-intel-hda,id=hda -device hda-duplex,bus=hda.0,audiodev=n -audiodev none,id=n" \
    "audiotest capture 48000; audiotest capture 144000; audiotest play 48000" ""
run ich6-output-bitexact \
    "-device intel-hda,id=hda -device hda-output,bus=hda.0,audiodev=w,mixer=off $WAV" \
    "audiotest play 96000" "pattern $D/out.wav 96000"
run ich9-intx-bitexact \
    "-device ich9-intel-hda,id=hda,msi=off -device hda-output,bus=hda.0,audiodev=w,mixer=off $WAV" \
    "audiotest play 96000" "pattern $D/out.wav 96000"
