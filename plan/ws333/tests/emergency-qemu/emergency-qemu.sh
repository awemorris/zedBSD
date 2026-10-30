#!/bin/bash
# The QEMU test of the thermal emergency (amd64, KVM when the host has it).
#   emergency-qemu.sh IMAGE OUT
# IMAGE is the raw disk image built from plan/ws333/tests/config-amd64-boot.mk.
#
# Boot 1 adds hot-zone.aml, an SSDT whose thermal zone reads 40 degrees for
# two minutes and then rises past its 90-degree critical level, and waits up
# to WAIT seconds (default 420) for the guest to turn its power off: QMP's
# SHUTDOWN event with the reason guest-shutdown (the ACPI S5 write; a reset
# or a crash gives another reason or none).  The screen is captured every 10
# seconds as OUT/boot1-NNN.png.  Boot 2 boots the disk boot 1 wrote, without
# the SSDT, through plan/tools/boot-test.sh, logs in as root on the keyboard
# and shows /var/log/thermal-emergency, the record init writes when the
# kernel tells it of the emergency (OUT/boot2/login-after.png).  Nothing
# reads a console or serial log.
#
# The summary is OUT/summary.txt; its last line is "emergency-qemu: PASS"
# or "emergency-qemu: FAIL (...)".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
image=${1:?usage: emergency-qemu.sh IMAGE OUT}
out=${2:?usage: emergency-qemu.sh IMAGE OUT}
wait_limit=${WAIT:-420}
mkdir -p "$out"
summary=$out/summary.txt
: >"$summary"

fail()
{
	echo "emergency-qemu: FAIL ($1)" | tee -a "$summary"
	exit 1
}

# The SSDT.
iasl -p "$out/hot-zone" "$here/hot-zone.asl" >"$out/iasl.log" 2>&1 || fail "iasl"

# Boot 1 on an overlay, so boot 2 sees what boot 1 wrote and the image stays as built.
qemu-img create -q -f qcow2 -F raw -b "$(readlink -f -- "$image")" "$out/disk.qcow2" || fail "overlay"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/vars.fd" || fail "firmware variables"
. "$root/plan/tools/guest/qemu-accel.sh"
read -r -a accel <<<"$(qemu_accel_args max)"
echo "boot 1: accel ${accel[*]}" >>"$summary"
timeout $((wait_limit + 60)) qemu-system-x86_64 -machine q35 -m 4G -smp 4 "${accel[@]}" \
	-drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
	-drive "if=pflash,format=raw,file=$out/vars.fd" \
	-device qemu-xhci,id=xhci \
	-drive "if=none,id=boot,file=$out/disk.qcow2,format=qcow2" \
	-device usb-storage,bus=xhci.0,port=1,drive=boot,bootindex=1 \
	-device usb-kbd,bus=xhci.0,port=2 \
	-acpitable "file=$out/hot-zone.aml" \
	-vga std -display none -no-reboot -no-shutdown \
	-qmp "unix:$out/qmp1.sock,server,nowait" >"$out/qemu1.out" 2>&1 &
qemu=$!
watch=$(python3 -I "$here/qmp-watch.py" "$out/qmp1.sock" "$out" boot1 "$wait_limit" 10)
echo "boot 1: $watch" >>"$summary"
python3 -I "$root/plan/tools/qmp.py" "$out/qmp1.sock" quit >/dev/null 2>&1
wait "$qemu" 2>/dev/null
case $watch in
"shutdown guest-shutdown "*) ;;
*) fail "boot 1 did not turn its power off: $watch" ;;
esac

# Boot 2 without the SSDT: the record init wrote.
qemu-img convert -O raw "$out/disk.qcow2" "$out/boot2.img" || fail "convert"
OUTPUT=$out/boot2 BOOT_MODE=uefi-usb BOOT_TIMEOUT=240 \
	BOOT_THEN=$'keys:root\nexpect:#\nkeys:cat /var/log/thermal-emergency\nexpect:thermal emergency at [0-9]+, [0-9]+ ms after init started' \
	timeout 400 "$root/plan/tools/boot-test.sh" "$out/boot2.img" >"$out/boot2.out" 2>&1
status=$?
echo "boot 2: boot-test status $status, screen $out/boot2/login-after.png" >>"$summary"
[ "$status" -eq 0 ] || fail "boot 2 did not show the record"
echo "emergency-qemu: PASS" | tee -a "$summary"
