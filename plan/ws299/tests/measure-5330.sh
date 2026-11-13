#!/bin/sh
# ws299: the whole NVMe comparison on the Latitude 5330 in one run, under /tmp/i915-hw.lock: Debian's modes, then
# VMUNIX on p4 with zedbsd.cfg plus watchdog=WATCHDOG and zedBSD's modes (each a single start, ~/boot-zedbsd.sh),
# then p4 put back as it was and the two compared.
#   sh plan/ws299/tests/measure-5330.sh NAME VMUNIX
# DMODES (default "read write p7raw p7fs p7direct") are bench-debian.sh's, ZMODES (default "read write p7rawwrite
# p7rawread p7fswrite p7fsread p7direct") bench-zedbsd.sh's; either may be empty.  The 5330 must be on Debian with
# p4's kernel and zedbsd.cfg kept as ~/ws245/vmunix-p027-orig and ~/ws245/zedbsd.cfg-p027-orig (their sha256 are
# checked before and after).  A zedBSD that stops is reset by the TCO watchdog (WATCHDOG s, default 600) and comes
# back as Debian; p4 is put back in every case.
# Output: LOGS (default build/hw5330/ws299) NAME-D-MODE.txt and NAME-Z-MODE.txt (nvme-bench-5330.sh), the table of
# compare.py in LOGS/NAME-compare.md, and LOGS/NAME.txt whose last line is "measure-5330: done" or
# "measure-5330: FAIL ...".  Nothing is removed on either machine.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
NAME=${1:?NAME}
VMUNIX=${2:?VMUNIX}
cd "$(dirname "$0")/../../.."
T=plan/ws299/tests
LOGS=${LOGS:-build/hw5330/ws299}
DMODES=${DMODES-read write p7raw p7fs p7direct}
ZMODES=${ZMODES-read write p7rawwrite p7rawread p7fswrite p7fsread p7direct}
WATCHDOG=${WATCHDOG:-600}
ORIG_VMUNIX=4c98d617279bf96b
ORIG_CFG=1b33a395b0ff4bd7
HOST=solaris10-man
NV_BOOT=/dev/disk/by-partuuid/d4dbe962-cc37-4f27-8529-bb7446730178
[ -f "$VMUNIX" ] || { echo "measure-5330: FAIL: no $VMUNIX"; exit 1; }
mkdir -p "$LOGS"
OUT=$LOGS/$NAME.txt
: > "$OUT"

say() {
	echo "measure-5330: $*" | tee -a "$OUT"
}
lin() {
	timeout "${TO:-120}" ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"
}
is_debian() {
	TO=8 lin hostname 2> /dev/null | grep -q chaos
}
# Waits for Debian; a zedBSD that stopped comes back after the watchdog's time and a restart.
debian_wait() {
	start=$(date +%s)
	until is_debian; do
		[ $(($(date +%s) - start)) -gt $((WATCHDOG + 600)) ] && return 1
		sleep 10
	done
	return 0
}
p4_sums() {
	TO=120 lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && sha256sum /mnt/zb/vmunix /mnt/zb/zedbsd.cfg; umount /mnt/zb'" |
		awk '{ printf "%s ", substr($1, 1, 16) }'
}
restore() {
	trap - EXIT
	if ! debian_wait; then
		say "FAIL: the 5330 is not on Debian $((WATCHDOG + 600)) s on; p4 still holds the measured kernel"
		exit 2
	fi
	TO=300 lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp \$HOME/ws245/vmunix-p027-orig /mnt/zb/vmunix && cp \$HOME/ws245/zedbsd.cfg-p027-orig /mnt/zb/zedbsd.cfg && sync && umount /mnt/zb'" > /dev/null 2>&1
	sums=$(p4_sums)
	if [ "$sums" = "$ORIG_VMUNIX $ORIG_CFG " ]; then
		say "p4 put back: vmunix $ORIG_VMUNIX, zedbsd.cfg $ORIG_CFG"
	else
		say "FAIL: p4 after the put back: $sums"
		exit 1
	fi
	TO=60 lin 'sync'
}

exec 9> /tmp/i915-hw.lock
flock -w 3600 9 || { say "FAIL: the lock"; exit 1; }
export WS299_LOCKED=1
is_debian || { say "FAIL: the 5330 is not on Debian"; exit 1; }
orig=$(TO=30 lin 'sha256sum ws245/vmunix-p027-orig ws245/zedbsd.cfg-p027-orig' | awk '{ printf "%s ", substr($1, 1, 16) }')
[ "$orig" = "$ORIG_VMUNIX $ORIG_CFG " ] || { say "FAIL: the kept p4 files are $orig"; exit 1; }
before=$(p4_sums)
say "$NAME start $(date '+%F %T'), kernel $(sha256sum "$VMUNIX" | cut -c1-16), p4 now $before"
trap restore EXIT

if [ -n "$DMODES" ]; then
	# shellcheck disable=SC2086
	sh $T/nvme-bench-5330.sh debian "$NAME-D" $DMODES 2>&1 | grep '^nvme-bench:' | tee -a "$OUT"
fi

if [ -n "$ZMODES" ]; then
	TO=120 timeout 120 scp -q "$VMUNIX" "$HOST:ws245/vmunix-$NAME" || { say "FAIL: copy of the kernel"; exit 1; }
	lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-$NAME" > /dev/null ||
		{ say "FAIL: kernel install"; exit 1; }
	lin "sudo -n sh -c 'mkdir -p /mnt/zb && { while mountpoint -q /mnt/zb; do umount /mnt/zb; done; } && mount $NV_BOOT /mnt/zb && cp \$HOME/ws245/zedbsd.cfg-p027-orig /mnt/zb/zedbsd.cfg && echo watchdog=$WATCHDOG >> /mnt/zb/zedbsd.cfg && sync && umount /mnt/zb'" > /dev/null 2>&1 ||
		{ say "FAIL: zedbsd.cfg"; exit 1; }
	say "p4 for zedBSD: $(p4_sums)(watchdog=$WATCHDOG)"
	# shellcheck disable=SC2086
	sh $T/nvme-bench-5330.sh zedbsd "$NAME-Z" - $ZMODES 2>&1 | grep '^nvme-bench:' | tee -a "$OUT"
	for f in "$LOGS/$NAME"-Z-*-dmesg.txt; do
		[ -f "$f" ] && say "$(basename "$f"): $(grep -h 'watchdog: armed\|host memory buffer' "$f" | tr '\n' ';' | cut -c1-200)"
	done
fi

restore
python3 -I $T/compare.py "$LOGS" "$NAME-D" "$NAME-Z" > "$LOGS/$NAME-compare.md"
cat "$LOGS/$NAME-compare.md"
sync
say "done $(date '+%F %T')"
