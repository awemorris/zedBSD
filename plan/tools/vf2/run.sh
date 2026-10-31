#!/bin/sh
# Puts a vmunix on the VisionFive 2 board (/boot/boot0/vmunix, checked by
# reading it back), power-cycles the board through the SwitchBot plug, and
# reads dmesg again and again while it boots (the kernel's message ring is
# 32 KiB, so the start would be lost by the end) until the render node is
# up; the unique lines go to OUTDIR/dmesg.txt.
#   VF2_HOST=user@host VF2_PASSWORD=... plan/tools/vf2/run.sh VMUNIX OUTDIR
# The password comes from the environment, never from this file.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
vmunix=$1
out=$2
: "${VF2_HOST:?VF2_HOST=user@host}"
: "${VF2_PASSWORD:?VF2_PASSWORD}"
plug=${VF2_PLUG:-AC276E432AE6}
mkdir -p "$out"
ssh_run() { sshpass -e ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$VF2_HOST" "$@"; }
export SSHPASS="$VF2_PASSWORD"
sshpass -e scp -o StrictHostKeyChecking=no -o ConnectTimeout=5 "$vmunix" "$VF2_HOST:/tmp/vmunix.new" || exit 1
printf '%s\n' "$VF2_PASSWORD" | ssh_run "sudo -S sh -c 'cp /tmp/vmunix.new /boot/boot0/vmunix && sync'" 2>/dev/null || exit 1
sshpass -e scp -o StrictHostKeyChecking=no "$VF2_HOST:/boot/boot0/vmunix" "$out/vmunix.back" || exit 1
cmp "$vmunix" "$out/vmunix.back" || exit 1
echo deployed
python3 -I plan/tools/switchbot.py cycle "$plug" 10 >/dev/null
# SSH comes up within 40 s of power-on when the SD card reads well; a boot
# that misses it is cycled again (the user, 2026-10-21, while the card is
# flaky): up to 8 tries.
for try in 1 2 3 4 5 6 7 8; do
	for i in $(seq 1 10); do sleep 4; ssh_run true 2>/dev/null && break 2; done
	echo "no ssh after 40 s, cycling again"
	python3 -I plan/tools/switchbot.py cycle "$plug" 10 >/dev/null
done
: > "$out/dmesg-all.txt"
stages=
for i in $(seq 1 80); do
	ssh_run dmesg >> "$out/dmesg-all.txt" 2>/dev/null
	stages=$(ssh_run sysctl hw.gpu.powervr.stages 2>/dev/null)
	case "$stages" in *"render-node=ok"*) break;; esac
	sleep 2
done
echo "$stages"
awk '!seen[$0]++' "$out/dmesg-all.txt" > "$out/dmesg.txt"
echo "dmesg lines: $(wc -l < "$out/dmesg.txt")"
