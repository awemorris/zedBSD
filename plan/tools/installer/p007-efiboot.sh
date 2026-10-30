#!/bin/sh
# The UEFI boot entries through the kernel's firmware variable driver, in QEMU with OVMF and a writable
# copy of its variable store, and the boots that have no runtime services.
#   sh plan/tools/installer/p007-efiboot.sh IMAGE OLD_LOADER [BIOS_IMAGE]
# IMAGE is an SSH guest image with efiboot (plan/tools/installer/config-amd64-p007.mk).  Four UEFI boots run
# plan/tools/installer/p007-guest-steps.sh:
#   1 create  efiboot makes a zedBSD entry on the boot disk's ESP with BootNext, and reads it back
#   2 after   the guest restarted on the same variable store: the firmware booted that entry (BootCurrent), the
#             entry is kept, BootNext is used up; the entry is deleted
#   3 gone    restarted again: the entry stayed deleted
#   4 nouefi  IMAGE with OLD_LOADER (a loader built before the runtime handoff) on its ESP: the kernel boots and
#             efiboot answers that the variables are not supported
# With BIOS_IMAGE (an image that also boots from the PC BIOS, ZEDBSD_VARIANT=hybrid), a fifth boot runs nouefi under
# SeaBIOS, from an IDE disk of the PC machine (SeaBIOS does not boot the image from NVMe).  The summary is the
# guest's STEP lines and "p007-efiboot: PASS|FAIL OUT".  Output goes to a new
# build/installer-test/p007-efiboot.*.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../.."
image=${1:?usage: p007-efiboot.sh IMAGE OLD_LOADER [BIOS_IMAGE]}
old_loader=${2:?usage: p007-efiboot.sh IMAGE OLD_LOADER [BIOS_IMAGE]}
bios_image=${3:-}
mkdir -p build/installer-test
out=$(mktemp -d build/installer-test/p007-efiboot.XXXXXX)
export GUEST_RUNTIME="$(pwd)/$out/guest"
g() { timeout 900 python3 plan/tools/guest/guest.py "$@"; }

# One UEFI boot of DISK on the variable store VARS, running the steps in MODE; the store is kept as SAVE.
boot() {
	disk=$1 vars=$2 save=$3 mode=$4 number=${5:-}
	g start "$disk" --ovmf-vars "$vars" > "$out/start-$mode.txt" 2>&1
	if g wait --timeout 300 > "$out/wait-$mode.txt" 2>&1; then
		g put plan/tools/installer/p007-guest-steps.sh /tmp/p007-steps.sh > /dev/null 2>&1 || true
		g run sh /tmp/p007-steps.sh "$mode" "$number" > "$out/steps-$mode.txt" 2>&1 || true
	else
		echo "STEP $mode-boot fail (no SSH answer)" > "$out/steps-$mode.txt"
	fi
	g stop > /dev/null 2>&1 || true
	cp "$GUEST_RUNTIME/uefi-vars.fd" "$save"
}

# 1-3: make the entry, restart into it and delete it, restart again.
boot "$image" /usr/share/OVMF/OVMF_VARS_4M.fd "$out/vars-1.fd" create
entry=$(sed -n 's/^ENTRY \([0-9A-F]\{4\}\)$/\1/p' "$out/steps-create.txt")
boot "$image" "$out/vars-1.fd" "$out/vars-2.fd" after "$entry"
boot "$image" "$out/vars-2.fd" "$out/vars-3.fd" gone "$entry"

# 4: the same image with the older loader on its ESP (the first partition of type EFI System).
cp --sparse=always "$image" "$out/old.img"
esp=$(python3 - "$out/old.img" <<'PY'
import struct, sys, uuid
with open(sys.argv[1], "rb") as disk:
	disk.seek(512)
	header = disk.read(92)
	entries, count, size = struct.unpack_from("<QII", header, 72)
	disk.seek(entries * 512)
	for index in range(count):
		entry = disk.read(size)
		if uuid.UUID(bytes_le=entry[:16]) == uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"):
			print(struct.unpack_from("<Q", entry, 32)[0] * 512)
			break
PY
)
mcopy -o -i "$out/old.img@@$esp" "$old_loader" ::/EFI/BOOT/BOOTX64.EFI
boot "$out/old.img" /usr/share/OVMF/OVMF_VARS_4M.fd "$out/vars-old.fd" nouefi

# 5: the PC BIOS, without a variable store, with the harness's own SSH (guest.py boots through OVMF only).
if [ -n "$bios_image" ]; then
	cp --sparse=always "$bios_image" "$out/bios.img"
	port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
	timeout 900 qemu-system-x86_64 -accel kvm -machine pc -m 4096 -smp 2 -cpu host \
		-device qemu-xhci,id=xhci,p2=8,p3=8 \
		-drive "if=ide,bus=0,unit=0,file=$out/bios.img,format=raw" \
		-netdev "user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dhcpstart=10.0.2.15,dns=10.0.2.3,hostfwd=tcp:127.0.0.1:$port-:22" \
		-device usb-net,bus=xhci.0,port=2,id=ecm,netdev=net0,mac=52:54:00:33:00:01,msos-desc=on \
		-vga std -display none -no-reboot -qmp "unix:$out/qmp-bios.sock,server,nowait" > "$out/qemu-bios.txt" 2>&1 &
	qemu=$!
	ssh_guest() {
		ssh -i plan/tmp/guest/id_ed25519 -p "$port" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
			-o LogLevel=ERROR -o ConnectTimeout=5 -o BatchMode=yes root@127.0.0.1 "$@"
	}
	answered=
	for _ in $(seq 150); do
		if ssh_guest true 2> /dev/null; then
			answered=y
			break
		fi
		sleep 2
	done
	if [ -n "$answered" ]; then
		scp -i plan/tmp/guest/id_ed25519 -P "$port" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
			-o LogLevel=ERROR plan/tools/installer/p007-guest-steps.sh root@127.0.0.1:/tmp/p007-steps.sh
		ssh_guest sh /tmp/p007-steps.sh nouefi | sed 's/^STEP /STEP bios-/; s/^P007 DONE nouefi/P007 DONE bios/' \
			> "$out/steps-bios.txt" 2>&1 || true
	else
		echo "STEP bios-boot fail (no SSH answer, screen in $out/bios-screen.ppm)" > "$out/steps-bios.txt"
		python3 - "$out/qmp-bios.sock" "$(pwd)/$out/bios-screen.ppm" <<'PY' || true
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.recv(65536)
s.send(b'{"execute":"qmp_capabilities"}\n')
time.sleep(0.5)
s.recv(65536)
s.send(json.dumps({"execute": "screendump", "arguments": {"filename": sys.argv[2]}}).encode() + b"\n")
time.sleep(2)
PY
	fi
	kill "$qemu" 2> /dev/null || true
	wait "$qemu" 2> /dev/null || true
fi

# The summary: every step, and PASS when every boot finished its steps without a failure.
status=0
for steps in "$out"/steps-*.txt; do
	grep -E '^(ENTRY|ESP|STEP|P007)' "$steps" || true
	grep -q '^P007 DONE' "$steps" || status=1
	if grep -q '^STEP .* fail' "$steps"; then
		status=1
	fi
done
if [ $status -eq 0 ]; then
	echo "p007-efiboot: PASS $out"
else
	echo "p007-efiboot: FAIL $out"
fi
exit $status
