#!/bin/sh
# BUG-382: discard p7 from Debian, boot zedBSD from the NVMe, run mkfs-probe.sh as root as soon as SSH answers,
# back to Debian; CYCLES times, under /tmp/i915-hw.lock.
#   sh plan/bugs/BUG-382/repro-5330.sh NAME CYCLES [VMUNIX]
# VMUNIX (when given) goes to p4 first (5330-nvme-write.sh kernel, the one before is kept as vmunix.old).
# LOOP (default 120) is the probe's seconds, LOAD=1 its sibling load, NEG=1 its refusal checks (mkfs-probe.sh).
# Output: build/bug382/NAME-N.txt and NAME-N-dmesg.txt per cycle; the last line is "repro: cycles=C first-fail=F
# try-fail=T/N".  Only p7 (zedBSD-nvme-bench) is discarded and formatted; nothing is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
NAME=${1:?NAME}
CYCLES=${2:?CYCLES}
KERNEL=${3:-}
cd "$(dirname "$0")/../../.."
B=plan/bugs/BUG-382
LOGS=build/bug382
HOST=solaris10-man
ZB=10.0.30.3
SO='-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR'
mkdir -p "$LOGS"
say() {
	echo "repro: $*"
}
lin() {
	timeout "${TO:-120}" ssh -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"
}
ZBU=user
zb() {
	timeout "${TO:-60}" sshpass -p "$ZBU" ssh $SO "$ZBU@$ZB" "$1"
}
debian_up() {
	start=$(date +%s)
	until TO=8 lin hostname 2> /dev/null | grep -q chaos; do
		if [ $(($(date +%s) - start)) -gt 300 ]; then
			say "FAIL: Debian did not come back in 5 minutes"
			exit 2
		fi
		sleep 5
	done
}
exec 9> /tmp/i915-hw.lock
flock 9
debian_up
TO=60 timeout 60 scp -q plan/ws299/tests/bench-debian.sh plan/tools/hw5330/5330-nvme-write.sh "$HOST:ws245/" ||
	{ say "FAIL: copy"; exit 1; }
lin 'sudo -n sh $HOME/ws245/5330-nvme-write.sh check' > /dev/null || { say "FAIL: partitions"; exit 1; }
if [ -n "$KERNEL" ]; then
	TO=120 timeout 120 scp -q "$KERNEL" "$HOST:ws245/vmunix-$NAME" || { say "FAIL: kernel copy"; exit 1; }
	lin "sudo -n sh \$HOME/ws245/5330-nvme-write.sh kernel \$HOME/ws245/vmunix-$NAME" > /dev/null ||
		{ say "FAIL: kernel install"; exit 1; }
	say "kernel installed ($(sha256sum "$KERNEL" | cut -c1-16))"
fi
entry=$(lin "sudo -n efibootmgr -v" | grep -i 08ab575f-f718-4eff-a3c9-10d80b1217ea | grep -i 'zedbsd.BOOTX64' |
    sed -n 's/^Boot\([0-9A-F]\{4\}\).*/\1/p' | head -1)
[ -n "$entry" ] || { say "FAIL: no UEFI entry"; exit 1; }
firsts=0
tries=0
fails=0
c=1
while [ "$c" -le "$CYCLES" ]; do
	log=$LOGS/$NAME-$c.txt
	lin 'sudo -n sh $HOME/ws245/bench-debian.sh p7fresh' > /dev/null || { say "FAIL: p7 fresh"; exit 1; }
	lin "sudo -n efibootmgr -n $entry > /dev/null && sudo -n systemctl reboot" > /dev/null 2>&1
	t0=$(date +%s)
	sleep 15
	until TO=10 zb 'uname -s' 2> /dev/null | grep -qi zedbsd; do
		if [ $(($(date +%s) - t0)) -gt 300 ]; then
			say "FAIL: zedBSD did not answer in 5 minutes (cycle $c; no remote power)"
			exit 2
		fi
		sleep 2
	done
	up=$(($(date +%s) - t0))
	TO=30 timeout 30 sshpass -p "$ZBU" scp $SO $B/mkfs-probe.sh "$ZBU@$ZB:/tmp/" || say "copy to zedBSD failed"
	TO=900 zb "echo $ZBU | sudo -S env LOOP=${LOOP:-120} LOAD=${LOAD:-0} NEG=${NEG:-0} sh /tmp/mkfs-probe.sh" > "$log" 2>&1
	TO=60 zb 'dmesg' > "$LOGS/$NAME-$c-dmesg.txt" 2>&1
	first=$(grep -o 'first .*' "$log" | head -1)
	probe=$(grep -o 'probe tries.*' "$log" | head -1)
	say "cycle $c: ssh up after ${up} s; $first; $probe"
	case "$first" in "first ok"*) ;; *) firsts=$((firsts + 1)) ;; esac
	n=$(echo "$probe" | sed -n 's/.*tries=\([0-9]*\).*/\1/p')
	f=$(echo "$probe" | sed -n 's/.*fails=\([0-9]*\).*/\1/p')
	tries=$((tries + ${n:-0}))
	fails=$((fails + ${f:-0}))
	zb "echo $ZBU | sudo -S reboot" > /dev/null 2>&1
	sleep 20
	debian_up
	c=$((c + 1))
done
say "cycles=$CYCLES first-fail=$firsts try-fail=$fails/$tries"
