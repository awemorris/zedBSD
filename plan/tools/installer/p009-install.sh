#!/bin/sh
# Install-admin in the installer's image.  Boots IMAGE (make installer-image with
# plan/tools/installer/config-amd64-p009.mk and the harness's files) from USB with four empty disks (NVMe 512, SATA, USB,
# NVMe 4096), runs plan/tools/installer/p009-guest-steps.sh in it over SSH (the probe, a refused live disk, three whole-disk
# installs and an install on partitions the editor's requests made, each installed system read back, the j3 sync rule
# on the SATA root left mounted), stops the guest, reads the synced file on the host through the loader's UFS reader,
# then boots each installed disk alone: the SATA disk (on NVMe), the USB disk and the 4096 disk to the console's login
# prompt (plan/tools/boot-test.sh), and the NVMe disk (the desktop, a graphical login) and the SATA disk over SSH as
# the installed user (the harness's key, which the steps put there), with a screen of the NVMe disk's.  The summary is
# the steps' lines and "p009-install: PASS|FAIL OUT".  Output goes to a new build/installer-test/p009.*.
#   sh plan/tools/installer/p009-install.sh IMAGE
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../.."
image=${1:?usage: p009-install.sh IMAGE}
mkdir -p build/installer-test
out=$(mktemp -d build/installer-test/p009.XXXXXX)
export GUEST_RUNTIME="$(pwd)/$out/guest"
truncate -s 8G "$out/nvme.img"
truncate -s 9G "$out/sata.img"
truncate -s 10G "$out/usb.img"
truncate -s 8G "$out/4k.img"
cp --sparse=always "$image" "$out/boot.img"
g() { timeout "${G_TIMEOUT:-1800}" python3 plan/tools/guest/guest.py "$@"; }
extra="-drive file=$out/nvme.img,if=none,id=p9n,format=raw -device nvme,serial=p009n,drive=p9n"
extra="$extra -device ahci,id=p9ahci -drive file=$out/sata.img,if=none,id=p9s,format=raw -device ide-hd,drive=p9s,bus=p9ahci.0"
extra="$extra -device qemu-xhci,id=p9x -drive file=$out/usb.img,if=none,id=p9u,format=raw -device usb-storage,bus=p9x.0,drive=p9u"
extra="$extra -drive file=$out/4k.img,if=none,id=p9k,format=raw -device nvme,serial=p009k,drive=p9k,logical_block_size=4096,physical_block_size=4096"
status=0
g start "$out/boot.img" --disk usb --qemu-extra "$extra" > "$out/start.txt" 2>&1
if g wait --timeout 600 > "$out/wait.txt" 2>&1; then
	g put plan/tools/installer/p009-guest-steps.sh /tmp/p009-steps.sh > /dev/null 2>&1 || true
	G_TIMEOUT=5400 g run sh /tmp/p009-steps.sh > "$out/steps.txt" 2>&1 || true
else
	echo "p009: the live system did not answer" > "$out/steps.txt"
fi
g stop > /dev/null 2>&1 || true
grep -E '^(STEP|OUT|LIVE|USB|INSTALLED|UNITS|FSTAB|SWAPFILE|LEFT|CFG|SYNC|P009)' "$out/steps.txt" || true
grep -q '^P009 DONE' "$out/steps.txt" || status=1
grep -q '^STEP .* fail' "$out/steps.txt" && status=1
grep -q '^LEFT ' "$out/steps.txt" && { echo "HOST install-admin left on an installed root"; status=1; }

# The synced file, read on the host through the loader's reader (512-byte blocks).
cc -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -I. -o "$out/ufs-cat" plan/tools/installer/host/ufs-cat.c bootloader/common/ufs-read.c
start=$(awk '$1 == "SYNCROOT" { print $3 }' "$out/steps.txt")
want=$(awk '$1 == "SYNCSUM" { print $3 " " $4 }' "$out/steps.txt")
if [ -n "$start" ] && [ -n "$want" ]; then
	host=$("$out/ufs-cat" "$out/sata.img" $((start * 512)) 512 /boot/p009-sync.bin 2> "$out/ufs-cat.txt" | cksum) || host=
	if [ "$host" = "$want" ]; then
		echo "HOST j3-sync ok"
	else
		echo "HOST j3-sync fail ($host, want $want)"; status=1
	fi
else
	echo "HOST j3-sync fail (no sum)"; status=1
fi

# Each installed disk alone, to the console's login prompt.
boot() {
	name=$1; mode=$2; shift 2
	env BOOT_MODE=$mode OUTPUT="$out/boot-$name" BOOT_TIMEOUT=240 "$@" \
		timeout 500 bash plan/tools/boot-test.sh "$out/$name.img" > "$out/boot-$name.txt" 2>&1 && r=0 || r=$?
	echo "BOOT $name $mode rc=$r $(tail -1 "$out/boot-$name.txt")"
	[ $r -eq 0 ] || status=1
}
boot sata uefi-nvme
boot usb uefi-usb
boot 4k uefi-nvme BOOT_NVME_BLOCK_SIZE=4096

# The NVMe and SATA disks alone over SSH, as the installed user.
ssh_check() {
	name=$1
	export GUEST_RUNTIME="$(pwd)/$out/guest-$name"
	g start "$out/$name.img" --disk nvme > "$out/ssh-$name-start.txt" 2>&1 || true
	port=$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["ssh_port"])' "$GUEST_RUNTIME/session.json" 2>/dev/null || echo 0)
	answer=
	n=0
	while [ $n -lt 90 ]; do
		answer=$(timeout 20 ssh -i plan/tmp/guest/id_ed25519 -p "$port" -o BatchMode=yes -o StrictHostKeyChecking=no \
			-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 tester@127.0.0.1 \
			'id -un; hostname; cat /var/db/installer/units | cut -f1 | tr "\n" " "; ls -ld /home/tester' 2> /dev/null) && break
		answer=
		sleep 5
		n=$((n + 1))
	done
	g screenshot "$out/ssh-$name.png" > /dev/null 2>&1 || true
	g stop > /dev/null 2>&1 || true
	echo "SSH $name $(printf '%s' "$answer" | tr '\n' '|')"
	case $answer in tester*p009host*) ;; *) status=1 ;; esac
}
ssh_check nvme
ssh_check sata
[ $status -eq 0 ] && echo "p009-install: PASS $out" || echo "p009-install: FAIL $out"
exit $status
