#!/bin/bash
# The amd64 tick under QEMU+KVM: boots IMAGE (vmunix.map beside it), reads the
# kernel's kernel_ticks twice 10 s apart through an HMP monitor and prints the
# rate, then the LAPIC timer mode (periodic or tsc-deadline) of CPUs 0-3.
#   tick-rate.sh OUTDIR CPU IMAGE     CPU: host (invariant TSC hidden: periodic)
#                                          or host,+invtsc (TSC-deadline)
# The console is not read.  Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
d=$1; cpu=$2; image=$3
address=$(awk '$NF == "kernel_ticks" { print $1; exit }' "$(dirname -- "$image")/vmunix.map")
[ -n "$address" ] || { echo "tick-rate: no kernel_ticks in vmunix.map" >&2; exit 1; }
mkdir -p "$d"
qemu-img create -q -f qcow2 -F raw -b "$image" "$d/disk.qcow2"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$d/vars.fd"
timeout 90 qemu-system-x86_64 -machine q35 -m 8G -smp 4 -accel kvm -cpu "$cpu" \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	-drive if=pflash,format=raw,file="$d/vars.fd" \
	-drive if=none,id=boot,file="$d/disk.qcow2",format=qcow2 \
	-device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
	-vga std -display none -no-reboot \
	-monitor unix:"$d/m.sock",server,nowait >"$d/qemu.out" 2>&1 &
pid=$!
hmp() {
	printf '%b' "$1" | timeout 5 socat -t 1 - UNIX-CONNECT:"$d/m.sock" 2>/dev/null
}
read_ticks() {
	hmp "x /1gx 0x$address\n" | grep -a -o "$address: 0x[0-9a-f]*" | awk '{ print $2 }'
}
sleep 25
t0=$(date +%s.%N); a=$(read_ticks)
sleep 10
t1=$(date +%s.%N); b=$(read_ticks)
hmp 'cpu 0\ninfo lapic\ncpu 1\ninfo lapic\ncpu 2\ninfo lapic\ncpu 3\ninfo lapic\n' |
	grep -a -P '^LVTT\t' | awk '{ print "LVTT", $5 }' | sort | uniq -c > "$d/lvtt.txt"
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
python3 -I -c 'import sys
a, b, t0, t1 = int(sys.argv[1], 16), int(sys.argv[2], 16), float(sys.argv[3]), float(sys.argv[4])
print(f"tick-rate: cpu={sys.argv[5]} {(b - a) / (t1 - t0):.1f} ticks/s over {t1 - t0:.2f} s")' "$a" "$b" "$t0" "$t1" "$cpu"
sed 's/^/tick-rate: /' "$d/lvtt.txt"
