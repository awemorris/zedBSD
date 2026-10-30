#!/bin/bash
# The x86 processor driver under QEMU+KVM (-cpu host, a hypervisor): builds the
# image of CONFIG (default plan/ws333/tests/config-amd64-cpuperf.mk), boots it
# with plan/tools/boot-test.sh to the login prompt, logs in as root on the
# emulated keyboard and shows the driver's lines of the kernel log
# (OUT/boot/login-after.png), then boots it again and reads through the HMP
# monitor the provider the CPU performance framework holds and the driver's
# operations (vmunix.map beside the image).  Under a hypervisor the framework
# must hold the driver's provider with the description only: topology and
# describe set, every operation that touches a register NULL.
#   cpu-x86-qemu.sh OUT [CONFIG]
# Last line: cpu-x86-qemu: PASS or cpu-x86-qemu: FAIL.  The console is not read.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
repo=$(cd "$(dirname "$0")/../../.." && pwd)
out=${1:?out dir}
config=${2:-plan/ws333/tests/config-amd64-cpuperf.mk}
mkdir -p "$out"
cd "$repo" || exit 1
status=0

# Builds the image (one image build at a time on the host).
if ! timeout 3600 flock /tmp/zedbsd-image-build.lock \
	make -j24 ZEDBSD_CONFIG="$config" BUILD="$out/img" disk-image > "$out/build.log" 2>&1; then
	echo "cpu-x86-qemu: image build failed ($out/build.log)"
	echo "cpu-x86-qemu: FAIL"
	exit 1
fi
image=$out/img/hdd-image.img
echo "cpu-x86-qemu: kernel warnings $(grep -c 'warning:' "$out/build.log") in the image build log (userland included)"

# Boots to the login prompt and shows the driver's lines.
if OUTPUT="$out/boot" BOOT_TIMEOUT=300 \
	BOOT_THEN=$'keys:root\nexpect:Password:\nkeys:root\nexpect:root@\nkeys:dmesg | grep cpu-x86\nexpect:cpu-x86: family' \
	timeout 900 plan/tools/boot-test.sh "$image" > "$out/boot-test.out" 2>&1; then
	echo "cpu-x86-qemu: boot-test PASS ($out/boot/login-after.png)"
else
	echo "cpu-x86-qemu: boot-test FAIL ($out/boot-test.out)"
	status=1
fi

# Boots again and reads the framework's provider and the driver's operations.
map=$out/img/vmunix.map
[ -f "$map" ] || map=$(find "$out/img" -name vmunix.map | head -1)
symbol() { awk -v name="$1" '$NF == name { print $1; exit }' "$map"; }
holder=$(symbol cpuperf_provider)
ops=$(symbol cpu_x86_provider)
if [ -z "$holder" ] || [ -z "$ops" ]; then
	echo "cpu-x86-qemu: symbols missing from $map"
	echo "cpu-x86-qemu: FAIL"
	exit 1
fi
d=$out/hmp
mkdir -p "$d"
qemu-img create -q -f qcow2 -F raw -b "$image" "$d/disk.qcow2"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$d/vars.fd"
timeout 150 qemu-system-x86_64 -machine q35 -m 8G -smp 4 -accel kvm -cpu host \
	-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	-drive if=pflash,format=raw,file="$d/vars.fd" \
	-drive if=none,id=boot,file="$d/disk.qcow2",format=qcow2 \
	-device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
	-vga std -display none -no-reboot \
	-monitor unix:"$d/m.sock",server,nowait > "$d/qemu.out" 2>&1 &
pid=$!
hmp() { printf '%b' "$1" | timeout 5 socat -t 1 - UNIX-CONNECT:"$d/m.sock" 2>/dev/null; }
sleep 60
hmp "x /1gx 0x$holder\n" > "$d/holder.txt"
hmp "x /12gx 0x$ops\n" > "$d/ops.txt"
hmp "screendump $d/screen.ppm\n" > /dev/null
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
python3 -I - "$holder" "$ops" "$d/holder.txt" "$d/ops.txt" <<'PY'
import re, sys
holder, ops, holder_file, ops_file = sys.argv[1:5]
def words(path):
    values = []
    for line in open(path, errors="replace"):
        match = re.match(r"^([0-9a-f]{16}):((?:\s+0x[0-9a-f]+)+)", line.strip())
        if match:
            values += [int(word, 16) for word in match.group(2).split()]
    return values
held = words(holder_file)
table = words(ops_file)
names = ["topology", "describe", "start", "apply", "sample", "measure", "relative_sources",
         "relative", "feedback", "feedback_read", "idle_package", "idle_core"]
ok = len(held) == 1 and len(table) == 12
if ok:
    print(f"cpu-x86-qemu: cpuperf_provider = {held[0]:#x} (cpu_x86_provider at {int(ops, 16):#x})")
    for name, value in zip(names, table):
        print(f"cpu-x86-qemu:   {name} = {value:#x}")
    ok = held[0] == int(ops, 16) and table[0] != 0 and table[1] != 0 and all(v == 0 for v in table[2:])
print("cpu-x86-qemu: provider " + ("description only (the gate held)" if ok else "NOT as expected"))
sys.exit(0 if ok else 1)
PY
[ $? = 0 ] || status=1
if [ "$status" = 0 ]; then echo "cpu-x86-qemu: PASS"; else echo "cpu-x86-qemu: FAIL"; fi
exit "$status"
