#!/bin/sh
# ws049-p006: compares the kernel's ACPI namespace and device evaluations in a
# QEMU guest (read through /dev/acpi) with aml-host on the tables the same
# QEMU machine builds.
#   sh plan/ws049/tests/guest-compare.sh IMAGE
# IMAGE is an SSH guest image (plan/tools/guest/build-ssh-image.sh) whose
# kernel has CONFIG_DRIVER_ACPI.  Writes build/ws049/guest/*.txt and prints
# the differences.  The guest's console is not read.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
image=$1
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root" || exit 1
export GUEST_RUNTIME=${GUEST_RUNTIME:-build/ws049-guest-run}
guest=plan/tools/guest/guest.py
out=build/ws049/guest
host=build/aml-host/aml-host
rm -rf "$out"
mkdir -p "$out"

# The tables of a q35 machine like the guest's (QEMU builds them; the
# firmware only installs them, so SeaBIOS gives the same ones as OVMF).
python3 plan/ws049/tests/qemu-acpi-dump.py "$out/tables" -machine q35 -m 8192 -smp 4 \
	-vga std -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0,port=3 \
	-drive if=none,id=nv,file=null-co://,format=raw -device nvme,serial=zedbsd-boot,drive=nv \
	-serial null -parallel null > /dev/null || exit 1
tables="$out/tables/dsdt.dat"
for table in "$out"/tables/ssdt*.dat; do
	[ -f "$table" ] && tables="$tables $table"
done

# What aml-host makes of them: the namespace after initialization, and
# every device's identification and resources.
$host --quiet --init --reg --dump $tables > "$out/host-namespace.txt"
$host --quiet --init --reg --devices $tables > "$out/host-devices.txt"
sed -n 's/ = .*//p' "$out/host-devices.txt" > "$out/paths.txt"

# What the kernel made of them.
python3 $guest stop > /dev/null 2>&1
python3 $guest start --disk nvme "$image" > /dev/null || exit 1
python3 $guest wait > /dev/null || { echo "FAIL no SSH"; exit 1; }
python3 $guest put "$out/paths.txt" /root/acpi-paths.txt
python3 $guest run 'cat /dev/acpi > /root/acpi-namespace.txt;
	while read -r path; do
		sh -c "exec 3<>/dev/acpi; printf %s \"\$1\" >&3; cat <&3" sh "$path"
	done < /root/acpi-paths.txt > /root/acpi-devices.txt'
python3 $guest get /root/acpi-namespace.txt "$out/guest-namespace.txt"
python3 $guest get /root/acpi-devices.txt "$out/guest-devices.txt"
python3 $guest stop > /dev/null

# The differences.
echo "namespace: $(wc -l < "$out/host-namespace.txt") host, $(wc -l < "$out/guest-namespace.txt") guest"
diff "$out/host-namespace.txt" "$out/guest-namespace.txt" > "$out/namespace.diff" && echo "namespace: same"
echo "devices: $(wc -l < "$out/host-devices.txt") host, $(wc -l < "$out/guest-devices.txt") guest"
diff "$out/host-devices.txt" "$out/guest-devices.txt" > "$out/devices.diff" && echo "devices: same"
head -40 "$out/namespace.diff" "$out/devices.diff"
