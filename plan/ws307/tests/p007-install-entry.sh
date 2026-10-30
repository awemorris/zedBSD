#!/bin/sh
# WS307 p007: the boot entry the installer makes, in QEMU with OVMF and a writable copy of its variable store.
#   sh plan/ws307/tests/p007-install-entry.sh IMAGE
# IMAGE is the installer's image (make installer-image with plan/ws307/tests/config-amd64-p009.mk and the harness's
# files).  Two boots run plan/ws307/tests/p007-install-entry-steps.sh:
#   1 install  IMAGE from USB with one empty NVMe disk: stale entries made, a whole-disk install through install-admin,
#              the new entry read back, the stale one pruned, BootNext set to the new entry
#   2 booted   the installed NVMe disk alone on the variable store the first boot left: the firmware started it from
#              the new entry, which efiboot prune keeps
# The summary is the guest's lines and "p007-install-entry: PASS|FAIL OUT".  Output goes to a new p007i.* in
# build/ws307-test, or in the directory OUT_ROOT names (the boots copy the image and the disks: several GiB).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname "$0")/../../.."
image=${1:?usage: p007-install-entry.sh IMAGE}
root=${OUT_ROOT:-build/ws307-test}
mkdir -p "$root"
out=$(realpath "$(mktemp -d "$root/p007i.XXXXXX")")
g() { timeout "${G_TIMEOUT:-900}" python3 plan/tools/guest/guest.py "$@"; }
truncate -s 8G "$out/nvme.img"

# 1: the installer's live system from USB, the NVMe disk beside it.
export GUEST_RUNTIME="$out/guest-install"
extra="-drive file=$out/nvme.img,if=none,id=p7n,format=raw -device nvme,serial=p007n,drive=p7n"
g start "$image" --disk usb --ovmf-vars /usr/share/OVMF/OVMF_VARS_4M.fd --qemu-extra "$extra" > "$out/start-install.txt" 2>&1
if g wait --timeout 600 > "$out/wait-install.txt" 2>&1; then
	g put plan/ws307/tests/p007-install-entry-steps.sh /tmp/p007i-steps.sh > /dev/null 2>&1 || true
	G_TIMEOUT=3600 g run sh /tmp/p007i-steps.sh install > "$out/steps-install.txt" 2>&1 || true
else
	echo "STEP install-boot fail (no SSH answer)" > "$out/steps-install.txt"
fi
g stop > /dev/null 2>&1 || true
cp "$GUEST_RUNTIME/uefi-vars.fd" "$out/vars-1.fd"
new=$(awk '$1 == "ENTRY" && $2 == "new" { print $3 }' "$out/steps-install.txt")

# 2: the installed disk alone, on the store the first boot left.
export GUEST_RUNTIME="$out/guest-booted"
g start "$out/nvme.img" --disk nvme --ovmf-vars "$out/vars-1.fd" > "$out/start-booted.txt" 2>&1
if g wait --timeout 300 > "$out/wait-booted.txt" 2>&1; then
	g put plan/ws307/tests/p007-install-entry-steps.sh /tmp/p007i-steps.sh > /dev/null 2>&1 || true
	g run sh /tmp/p007i-steps.sh booted "$new" > "$out/steps-booted.txt" 2>&1 || true
else
	echo "STEP booted-boot fail (no SSH answer)" > "$out/steps-booted.txt"
fi
g screenshot "$out/booted.png" > /dev/null 2>&1 || true
g stop > /dev/null 2>&1 || true
cp "$GUEST_RUNTIME/uefi-vars.fd" "$out/vars-2.fd"

# The summary: every step, and PASS when both boots finished their steps without a failure.
status=0
for steps in "$out/steps-install.txt" "$out/steps-booted.txt"; do
	grep -E '^(DISKS|ENTRY|ESP|ROOT|OUT|STEP|PRUNED|P007I)' "$steps" || true
	grep -q '^P007I DONE' "$steps" || status=1
	if grep -q '^STEP .* fail' "$steps"; then
		status=1
	fi
done
if [ $status -eq 0 ]; then
	echo "p007-install-entry: PASS $out"
else
	echo "p007-install-entry: FAIL $out"
fi
exit $status
