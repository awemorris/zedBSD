#!/bin/sh
# The video encoder's self-test on the RTX 2070 machine (GPU_PERF_MACHINE=rtx, 10.0.10.3: Debian, zedBSD started
# once with UEFI BootNext), in one command, run from the top of the tree in the machine's lock (flock
# /tmp/zedbsd-rdna.lock):
#   1. zedBSD running; its NVMe kernel and boot line read back into OUT (nvme-vmunix, zedbsd.cfg) and their cksums kept
#   2. VMUNIX put in, nvrtx.video=1 and nvrtx.nvenc.test=1 on the boot line, zedBSD restarted through Debian
#   3. the driver's lines read by SSH (dmesg): the encoder's channel, the self-test (nvenc-test: each choice's two
#      pictures, their statuses, bitstreams and reconstructions in hexadecimal lines), channel errors, faults; the
#      desktop's session socket
#   4. the bitstreams checked on the host by plan/ws324/tests/nvenc-decode.py (ffmpeg, PSNR, PNG in OUT/decode)
#   5. the NVMe kernel and the boot line put back, checked by cksum against 1, and Debian running again (the RTX's
#      driver in use)
# OUT/summary.txt has what is printed; the logs stay in OUT.
#   usage: plan/ws324/tests/rtx-nvenc-test.sh OUT VMUNIX   (from the top of the tree)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
OUT=${1:?usage: rtx-nvenc-test.sh OUT VMUNIX}
VMUNIX=${2:?usage: rtx-nvenc-test.sh OUT VMUNIX}
export GPU_PERF_MACHINE=${GPU_PERF_MACHINE:-rtx}
MEASURE="sh plan/tools/gpu-perf/measure.sh"
top=$(pwd)
. plan/tools/gpu-perf/conf.sh
KEY=${KEY:-$top/plan/tmp/guest/id_ed25519}
SSHOPT="-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
mkdir -p "$OUT" "$top/build/tmp"
ZSSHOPT="$SSHOPT -o ControlMaster=auto -o ControlPath=$top/build/tmp/ssh-p1-%h -o ControlPersist=600"
SUMMARY=$OUT/summary.txt
: > "$SUMMARY"
KLINES="nvenc|nvdec|work-channel ok|channel error|executor fail|lost|fault|gsp-init (ok|fail)|rpc: message"

say() {
	echo "$(date +%H:%M:%S) $*" | tee -a "$SUMMARY"
}

# zs IP COMMAND: a command on zedBSD as root, at most ZS_TIMEOUT seconds.
zs() {
	zip=$1
	shift
	timeout "${ZS_TIMEOUT:-60}" ssh -i "$KEY" $ZSSHOPT "root@$zip" "$@"
}

# zcp SOURCE DEST: one file copied either way (scp's own form of the remote side).
zcp() {
	timeout 300 scp -O -i "$KEY" $ZSSHOPT "$1" "$2" > /dev/null 2>&1
}

deb() {
	timeout "${ZS_TIMEOUT:-60}" ssh $SSHOPT "$DEBIAN" "$@"
}

[ -f "$VMUNIX" ] || { say "FAIL no $VMUNIX"; exit 1; }
[ "$(stat -c %a "$KEY")" = 600 ] || { say "FAIL the key $KEY is not mode 600"; exit 1; }

# 1. zedBSD running, its kernel and boot line kept.
IP=$(timeout 900 $MEASURE zedbsd 2> "$OUT/zedbsd.log") || { say "FAIL zedBSD not up"; exit 1; }
zs "$IP" "uname -v; cksum < /boot/boot0/vmunix; cksum < /boot/boot0/zedbsd.cfg; grep -v '^#' /boot/boot0/zedbsd.cfg" > "$OUT/before.txt" 2>&1 < /dev/null
kernel_before=$(sed -n 2p "$OUT/before.txt" | cut -d' ' -f1)
cfg_before=$(sed -n 3p "$OUT/before.txt" | cut -d' ' -f1)
zcp "root@$IP:/boot/boot0/vmunix" "$OUT/nvme-vmunix" || { say "FAIL read back the NVMe kernel"; exit 1; }
zcp "root@$IP:/boot/boot0/zedbsd.cfg" "$OUT/zedbsd.cfg" || { say "FAIL read back the boot line"; exit 1; }
sync
kept_kernel=$(cksum < "$OUT/nvme-vmunix" | cut -d' ' -f1)
kept_cfg=$(cksum < "$OUT/zedbsd.cfg" | cut -d' ' -f1)
say "before: zedbsd $IP kernel $kernel_before (kept $kept_kernel) cfg $cfg_before (kept $kept_cfg)"
[ -n "$kernel_before" ] && [ "$kernel_before" = "$kept_kernel" ] && [ "$cfg_before" = "$kept_cfg" ] || { say "FAIL the kept copies differ; nothing changed"; exit 1; }

# 2. The kernel and the line in, zedBSD restarted.
zcp "$VMUNIX" "root@$IP:/tmp/vmunix.new" || { say "FAIL copy kernel"; exit 1; }
zs "$IP" "cp /tmp/vmunix.new /boot/boot0/vmunix && sed -i -e '/^nvrtx\\.video=/d' -e '/^nvrtx\\.nvenc\\.test=/d' /boot/boot0/zedbsd.cfg && echo nvrtx.video=1 >> /boot/boot0/zedbsd.cfg && echo nvrtx.nvenc.test=1 >> /boot/boot0/zedbsd.cfg && sync" < /dev/null || say "FAIL kernel and line"
timeout 900 $MEASURE linux > "$OUT/restart.log" 2>&1 || say "FAIL Debian not up"
IP=$(timeout 900 $MEASURE zedbsd 2>> "$OUT/zedbsd.log")
if [ -n "$IP" ]; then
	zs "$IP" "uname -v; cksum < /boot/boot0/vmunix; grep -E '^nvrtx\\.' /boot/boot0/zedbsd.cfg" > "$OUT/after.txt" 2>&1 < /dev/null
	say "under test: zedbsd $IP kernel $(sed -n 2p "$OUT/after.txt" | cut -d' ' -f1) (wanted $(cksum < "$VMUNIX" | cut -d' ' -f1)) | video line $(grep -c '^nvrtx.video=1' "$OUT/after.txt") | test line $(grep -c '^nvrtx.nvenc.test=1' "$OUT/after.txt")"

	# 3. The driver's lines.
	zs "$IP" "dmesg | grep 'nvrtx'" > "$OUT/nvrtx.log" 2>&1 < /dev/null
	grep -E "$KLINES" "$OUT/nvrtx.log" > "$OUT/lines.log"
	sleep 20
	zs "$IP" "test -S /run/user/1000/wayland-0 && echo session-socket" > "$OUT/session.txt" 2>&1 < /dev/null
	say "nvenc lines:"
	grep -E 'nvenc' "$OUT/nvrtx.log" | grep -v 'nvenc-dump' | cut -c1-160 | sed 's/^/    /' | tee -a "$SUMMARY"
	say "host decode:"
	/usr/bin/python3 -I plan/ws324/tests/nvenc-decode.py "$OUT/nvrtx.log" "$OUT/decode" 2>&1 | sed 's/^/    /' | tee -a "$SUMMARY"
	say "nvdec and work: $(grep -E 'nvdec-channel (ok|fail)' "$OUT/nvrtx.log" | tail -1 | cut -c1-120) | $(grep -c 'work-channel ok' "$OUT/nvrtx.log") work-channel ok"
	say "channel errors $(grep -c 'channel-stop channel error' "$OUT/nvrtx.log"), faults outside the boot's record of an earlier fault $(grep -i 'fault' "$OUT/nvrtx.log" | grep -vc 'before the tables'), lost $(grep -c 'lost' "$OUT/nvrtx.log"), session $(grep -c session-socket "$OUT/session.txt")"
	grep -E 'channel-stop channel error|fault' "$OUT/nvrtx.log" | grep -v 'before the tables' | head -6 | cut -c1-160 | sed 's/^/    /' | tee -a "$SUMMARY"
else
	say "FAIL zedBSD not up with the kernel under test"
fi

# 4. The machine given back: the kept kernel and line in, checked, then Debian.
IP=$(timeout 900 $MEASURE zedbsd 2>> "$OUT/zedbsd.log") || { say "FAIL zedBSD not up for the restore (the NVMe kernel is the one under test)"; exit 1; }
zcp "$OUT/nvme-vmunix" "root@$IP:/tmp/vmunix.back" || say "FAIL copy the kept kernel"
zcp "$OUT/zedbsd.cfg" "root@$IP:/tmp/zedbsd.cfg.back" || say "FAIL copy the kept boot line"
zs "$IP" "cp /tmp/vmunix.back /boot/boot0/vmunix && cp /tmp/zedbsd.cfg.back /boot/boot0/zedbsd.cfg && sync && cksum < /boot/boot0/vmunix && cksum < /boot/boot0/zedbsd.cfg" > "$OUT/restore.txt" 2>&1 < /dev/null
kernel_back=$(sed -n 1p "$OUT/restore.txt" | cut -d' ' -f1)
cfg_back=$(sed -n 2p "$OUT/restore.txt" | cut -d' ' -f1)
verdict=FAIL
[ "$kernel_back" = "$kernel_before" ] && [ "$cfg_back" = "$cfg_before" ] && verdict=MATCH
say "restore $verdict: kernel $kernel_back (before $kernel_before), cfg $cfg_back (before $cfg_before)"
timeout 900 $MEASURE linux >> "$OUT/restart.log" 2>&1 || say "FAIL Debian not up after the restore"
say "Debian: $(deb "uname -r; lspci -nnk -d 10de: | grep 'driver in use' | sort | uniq -c | tr -s ' '" 2> /dev/null | tr '\n' ' ')"
say "where: $(timeout 120 $MEASURE where 2> /dev/null)"
sync
say "done: $OUT"
