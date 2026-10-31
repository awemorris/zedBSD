#!/bin/sh
# The fork race regression in QEMU: builds the text login image with
# forkrace (userland/tests/forkrace), boots it, runs forkrace as root and
# prints one summary line, "forkrace-qemu: PASS" or "forkrace-qemu: FAIL".
#   sh plan/tools/process/forkrace-qemu.sh amd64|rpi4 BUILD OUT [ARGS]
# BUILD is the image's build directory, OUT a new directory for the
# build's and the run's output (the screens in OUT/boot on amd64).  ARGS
# go to forkrace (MEGABYTES ROUNDS LIMIT_MS; the default is "64 5 5000"
# on amd64 and "32 3 30000" on the raspi4b, which runs on TCG).  amd64
# boots the image through plan/tools/boot-test.sh, types on the emulated
# keyboard and reads the screen; rpi4 boots QEMU raspi4b, logs in with
# plan/tools/guest/serial.py and reads forkrace's answer over the
# interactive serial console.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
port=${1:?amd64 or rpi4}
build=${2:?build directory}
out=${3:?output directory}
shift 3
args=${*:-}
mkdir -p "$out"

case $port in
amd64)
	config=plan/tools/process/config-amd64-forkrace.mk
	test -n "$args" || args='64 5 5000'
	;;
rpi4)
	config=plan/tools/process/config-rpi4-forkrace.mk
	test -n "$args" || args='32 3 30000'
	;;
*)
	echo "forkrace-qemu: unknown port $port" >&2
	exit 2
	;;
esac

# Builds the image.
if ! timeout 3600 make -j16 ZEDBSD_CONFIG="$config" BUILD="$build" disk-image \
    >"$out/build.log" 2>&1; then
	echo "forkrace-qemu: FAIL build (see $out/build.log)"
	exit 1
fi

# Boots it and runs the test.
status=0
if [ "$port" = amd64 ]; then
	OUTPUT="$out/boot" BOOT_MODE=uefi-usb BOOT_TIMEOUT=240 \
	BOOT_THEN="keys:root
expect:Password:
keys:root
expect:root@
keys:forkrace $args
expect:FORKRACE PASS" \
	    timeout 900 plan/tools/boot-test.sh "$build/hdd-image.img" \
	    >"$out/boot-test.log" 2>&1 || status=$?
	if [ "$status" -eq 0 ]; then
		echo "forkrace-qemu: PASS amd64 ($out/boot/login-after.png)"
		exit 0
	fi
	echo "forkrace-qemu: FAIL amd64 status=$status ($out/boot/login-after.png, $out/boot-test.log)"
	exit 1
fi

# Boots QEMU raspi4b on a copy-on-write view of the card, its serial
# console on a socket in a new directory under /tmp (where the sandbox of
# an agent's shell lets one be made), and waits for the socket.
run=$(mktemp -d)
dtb=$(sh plan/tools/rpi4-dtb.sh "$build/hdd-image.img")
timeout 1500 qemu-system-aarch64 -M raspi4b -m 2G -kernel "$build/vmunix" \
    -dtb "$dtb" -drive "file=$build/hdd-image.img,format=raw,if=sd,snapshot=on" \
    -display none -serial "unix:$run/serial.sock,server=on,wait=off" \
    -serial null -monitor none >"$out/qemu.log" 2>&1 &
qemu=$!
trap 'kill "$qemu" 2>/dev/null || true' EXIT
waited=0
while [ ! -S "$run/serial.sock" ] && [ "$waited" -lt 60 ]; do
	sleep 1
	waited=$((waited + 1))
done

# Logs in on the serial console and runs forkrace, keeping what it printed.
serial="python3 plan/tools/guest/serial.py --socket $run/serial.sock --timeout 900"
timeout 1200 $serial run "forkrace $args" >"$out/forkrace.txt" 2>&1 || status=$?
grep 'forkrace \|FORKRACE' "$out/forkrace.txt" | tr -d '\r' >"$out/summary.txt"
if grep -q '^FORKRACE PASS' "$out/summary.txt"; then
	echo "forkrace-qemu: PASS rpi4 ($out/summary.txt)"
	exit 0
fi
echo "forkrace-qemu: FAIL rpi4 status=$status ($out/forkrace.txt)"
exit 1
