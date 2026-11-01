#!/bin/sh
# The Wave511 probe on the VisionFive 2 (ws322-p002), in one run: puts the streams (make-streams.sh), both firmware
# files and board-run.sh in /var/vpu-probe, boots KERNEL once through plan/tools/vf2/boot-cycle.sh with board-run.sh
# as its load, puts the board's own kernel back (boot-cycle.sh), removes what it put on the board and checks the
# board's files it must not change, then compares the probe's text with the references (compare.py).  With
# MODE=boot it instead installs chagall.bin at the driver's path before the boot, boots KERNEL (a kernel that brings
# the decoder up at boot) with boot-cycle.sh's GPU=1 checks, and reports the decoder's line of the boot's dmesg.
#   VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=... VF2_PLUG=AC276E432AE6 [MODE=boot] sh plan/ws322/tests/p002/run-vf2.sh KERNEL STREAMS FIRMWARE OUT
# STREAMS is make-streams.sh's output; FIRMWARE holds wave511_dec_fw.bin and chagall.bin.  The password comes from
# the environment, never from this file.  OUT/summary.txt is the summary.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
kernel=${1:?usage: run-vf2.sh KERNEL STREAMS FIRMWARE OUT}
streams=${2:?the streams}
firmware=${3:?the firmware directory}
out=${4:?the output directory}
: "${VF2_HOST:?}" "${VF2_PASSWORD:?}" "${VF2_PLUG:?}"
here=$(dirname "$0")
export SSHPASS="$VF2_PASSWORD"
SSHOPT="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5"
mkdir -p "$out"
b() { timeout 120 sshpass -e ssh $SSHOPT "$VF2_HOST" "$@"; }
bsudo() { printf '%s\n' "$VF2_PASSWORD" | b "sudo -S sh -c '$1'" 2> /dev/null; }
bcp() { timeout 300 sshpass -e scp $SSHOPT "$@"; }
say() { echo "$*" | tee -a "$out/summary.txt"; }
: > "$out/summary.txt"
say "start $(date '+%F %H:%M') board $(b 'uname -sm' 2>&1)"
say "before $(b 'cksum /boot/boot0/vmunix /boot/boot0/vmunix.q1825orig /etc/group /boot/boot0/zedbsd.cfg' 2>&1 | tr '\n' ' ')"
b 'mkdir -p vpu-stage' || { say "FAIL no ssh"; exit 1; }
bcp "$streams"/*.h264 "$streams"/*.h265 "$firmware/wave511_dec_fw.bin" "$firmware/chagall.bin" "$here/board-run.sh" \
	"$VF2_HOST:vpu-stage/" || { say "FAIL copy"; exit 1; }
bsudo "mkdir -p /var/vpu-probe /lib/firmware/wave511 && cp vpu-stage/* /var/vpu-probe/ && sync"
say "staged $(b 'ls /var/vpu-probe | wc -l' 2>&1) files"
if [ "${MODE:-probe}" = boot ]; then
	bsudo "cp /var/vpu-probe/chagall.bin /lib/firmware/wave511/chagall.bin && sync"
	VF2_HOST=$VF2_HOST VF2_PASSWORD=$VF2_PASSWORD VF2_PLUG=$VF2_PLUG ORIG=vmunix.q1825orig LIMIT=180 GPU=1 \
		sh plan/tools/vf2/boot-cycle.sh "$kernel" "$out/cycle" 1
else
	VF2_HOST=$VF2_HOST VF2_PASSWORD=$VF2_PASSWORD VF2_PLUG=$VF2_PLUG ORIG=vmunix.q1825orig LIMIT=180 \
		LOAD='sh /var/vpu-probe/board-run.sh' sh plan/tools/vf2/boot-cycle.sh "$kernel" "$out/cycle" 1
fi
cat "$out/cycle/summary.txt" >> "$out/summary.txt"
bsudo "rm -rf /var/vpu-probe /lib/firmware/wave511 && sync"
b 'rm -rf vpu-stage'
say "after $(b 'cksum /boot/boot0/vmunix /boot/boot0/vmunix.q1825orig /etc/group /boot/boot0/zedbsd.cfg; ls /var/vpu-probe /lib/firmware/wave511' 2>&1 | tr '\n' ' ')"
probe=$out/cycle/boot1/load.txt
if [ -f "$probe" ]; then
	awk '/=== probe firmware chagall.bin/{p=1;next} /=== probe firmware wave511_dec_fw.bin/{p=2;next} /=== end/{p=0}
		p==1{print > "'"$out"'/probe-chagall.txt"} p==2{print > "'"$out"'/probe-dec-fw.txt"}' "$probe"
	for f in probe-chagall probe-dec-fw; do
		[ -f "$out/$f.txt" ] || continue
		say "== $f"
		/usr/bin/python3 -I "$here/compare.py" "$out/$f.txt" "$streams" | tee -a "$out/summary.txt" > /dev/null
	done
fi
if [ -f "$out/cycle/boot1/dmesg.txt" ]; then
	say "decoder: $(grep -E 'vf2-vpu|video decoder' "$out/cycle/boot1/dmesg.txt" | tr '\n' ' ')"
fi
say "end $(date '+%F %H:%M')"
