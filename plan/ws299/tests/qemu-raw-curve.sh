#!/bin/sh
# ws299-p004: QEMU stand-in for the 5330's raw read curve.  Beside the boot disk one sparse 6 GiB NVMe; the guest
# (8 GiB, so the buffer cache is 1 GiB) reads CHUNKS chunks of 256 MiB of it one after another with /bin/time dd
# bs=1m, then prints the read-ahead log lines.  With SAMPLES=N the host also takes N samples of every CPU's stack over
# the gdbstub while the last chunks run (a poor man's profile of where the time goes once the cache is full) and
# counts the innermost kernel functions.
#   sh plan/ws299/tests/qemu-raw-curve.sh IMAGE          (IMAGE built with CONFIG_PCAT_SERIAL_MIRROR=y; VMUNIX is its
#                                                         kernel, default the image's directory's vmunix)
# W is ${WS299_RUNS:-build/tmp}/qemu-raw-curve (a fresh run each time).  The last line is "qemu-raw-curve: done|FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
. plan/tools/guest/qemu-accel.sh
. plan/tools/fresh-out.sh
IMAGE=${1:?image}
VMUNIX=${VMUNIX:-$(dirname "$IMAGE")/vmunix}
CHUNKS=${CHUNKS:-16}
SAMPLES=${SAMPLES:-0}
GDB_PORT=${GDB_PORT:-22212}
mkdir -p "${WS299_RUNS:-build/tmp}"; fresh_out "${WS299_RUNS:-build/tmp}"/qemu-raw-curve
W=${WS299_RUNS:-build/tmp}/qemu-raw-curve
truncate -s 6G "$W/d.img"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$W/vars.fd"
timeout 1800 qemu-system-x86_64 -name p1-raw-curve -machine q35 -m 8G -smp 4 $(qemu_accel_args max) \
  -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
  -drive "if=pflash,format=raw,file=$W/vars.fd" \
  -drive "if=none,id=boot,file=$IMAGE,format=raw,snapshot=on" \
  -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
  -drive "if=none,id=d,file=$W/d.img,format=raw,readonly=on" \
  -device nvme,serial=zedbsd-curve,drive=d \
  -vga std -display none -no-reboot \
  -serial "unix:$W/serial.sock,server=on,wait=off" \
  -gdb "tcp:127.0.0.1:$GDB_PORT" > "$W/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2> /dev/null' EXIT
i=0
while [ ! -S "$W/serial.sock" ] && [ $i -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
S="python3 plan/tools/guest/serial.py --socket $W/serial.sock --timeout 1200"
$S expect 'login: ' > /dev/null || { echo "qemu-raw-curve: FAIL no login"; exit 1; }
$S login > /dev/null
command="i=0; while [ \$i -lt $CHUNKS ]; do echo chunk\$i; /bin/time dd if=/dev/nvme1n1 of=/dev/null bs=1024k skip=\$((i * 256)) count=256 2>&1 | grep -E '^(real|sys) '; i=\$((i + 1)); done; dmesg | grep 'buf: readahead' | tail -3"
if [ "$SAMPLES" -gt 0 ]; then
	(
		sleep "${SAMPLE_DELAY:-20}"
		n=0
		while [ $n -lt "$SAMPLES" ]; do
			timeout 20 gdb -q -batch -nx "$VMUNIX" -ex "target remote 127.0.0.1:$GDB_PORT" \
			    -ex 'thread apply all bt 6' -ex detach >> "$W/samples.txt" 2>&1
			n=$((n + 1))
			sleep 0.5
		done
	) &
	sampler=$!
fi
$S run "$command" | tr -d '\r' | tee "$W/guest.txt"
[ "$SAMPLES" -gt 0 ] && wait $sampler
if [ "$SAMPLES" -gt 0 ]; then
	echo "innermost frames (all CPUs, $SAMPLES samples):"
	grep -E '^#0 ' "$W/samples.txt" | sed -E 's/^#0 +(0x[0-9a-f]+ in )?([A-Za-z_0-9]+).*/\2/' | sort | uniq -c | sort -rn | head -15
	echo "any frame:"
	grep -E '^#[0-5] ' "$W/samples.txt" | sed -E 's/^#[0-9]+ +(0x[0-9a-f]+ in )?([A-Za-z_0-9]+).*/\2/' | sort | uniq -c | sort -rn | head -25
fi
echo "qemu-raw-curve: done"
