#!/bin/sh
# Gives the RTX 2070 machine back after an rtx-video324.sh run that was stopped before its own step 6: the NVMe
# kernel and the boot line rtx-video324.sh kept in KEPT (nvme-vmunix, zedbsd.cfg, before.txt) put back on the running
# zedBSD (started first when Debian runs), checked by cksum against before.txt, then Debian.  Run from the top of the
# tree in the machine's lock (flock /tmp/zedbsd-rdna.lock).
#   usage: plan/ws324/tests/rtx-restore324.sh KEPT
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
KEPT=${1:?usage: rtx-restore324.sh KEPT}
export GPU_PERF_MACHINE=${GPU_PERF_MACHINE:-rtx}
MEASURE="sh plan/tools/gpu-perf/measure.sh"
top=$(pwd)
. plan/tools/gpu-perf/conf.sh
KEY=${KEY:-$top/plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$top/build/tmp"
ZSSHOPT="$SSHOPT -o ControlMaster=auto -o ControlPath=$top/build/tmp/ssh-p1r-%h -o ControlPersist=600"
SUMMARY=$KEPT/restore-summary.txt

say() {
	echo "$(date +%H:%M:%S) $*" | tee -a "$SUMMARY"
}

zs() {
	zip=$1
	shift
	timeout 60 ssh -i "$KEY" $ZSSHOPT "root@$zip" "$@"
}

zcp() {
	timeout 300 scp -O -i "$KEY" $ZSSHOPT "$@" > /dev/null 2>&1
}

kernel_before=$(sed -n 2p "$KEPT/before.txt" | cut -d' ' -f1)
cfg_before=$(sed -n 3p "$KEPT/before.txt" | cut -d' ' -f1)
[ -n "$kernel_before" ] && [ -f "$KEPT/nvme-vmunix" ] && [ -f "$KEPT/zedbsd.cfg" ] || { say "FAIL nothing kept in $KEPT"; exit 1; }
IP=$(timeout 900 $MEASURE zedbsd 2>> "$KEPT/restore-zedbsd.log") || { say "FAIL zedBSD not up for the restore"; exit 1; }
zcp "$KEPT/nvme-vmunix" "root@$IP:/tmp/vmunix.back" || say "FAIL copy the kept kernel"
zcp "$KEPT/zedbsd.cfg" "root@$IP:/tmp/zedbsd.cfg.back" || say "FAIL copy the kept boot line"
zs "$IP" "cp /tmp/vmunix.back /boot/boot0/vmunix && cp /tmp/zedbsd.cfg.back /boot/boot0/zedbsd.cfg && sync && cksum < /boot/boot0/vmunix && cksum < /boot/boot0/zedbsd.cfg" > "$KEPT/restore.txt" 2>&1 < /dev/null
kernel_back=$(sed -n 1p "$KEPT/restore.txt" | cut -d' ' -f1)
cfg_back=$(sed -n 2p "$KEPT/restore.txt" | cut -d' ' -f1)
verdict=FAIL
[ "$kernel_back" = "$kernel_before" ] && [ "$cfg_back" = "$cfg_before" ] && verdict=MATCH
say "restore $verdict: kernel $kernel_back (before $kernel_before), cfg $cfg_back (before $cfg_before)"
timeout 900 $MEASURE linux >> "$KEPT/restore-restart.log" 2>&1 || say "FAIL Debian not up after the restore"
say "where: $(timeout 120 $MEASURE where 2> /dev/null)"
sync
