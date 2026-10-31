#!/bin/sh
# BUG-382: run as root on the 5330's zedBSD at once after boot.  The same mkfs as bench-zedbsd.sh p7fswrite (a
# refused run for the registration, then FORMAT), then refused runs once a second for LOOP seconds (nothing is
# written by a refused run).  Prints "first ok|fail ...", one "try" line per run, and "probe tries=N fails=M".
# LOAD=1 keeps the root UFS (p5, zedBSD's own) busy during the loop: 8 MiB written to /root/bug382.load and synced,
# over and over, as a sibling partition's I/O.
# NEG=1 then checks that real users of p7's sectors still refuse it: p7 held open by another process, and the whole
# disk nvme0n1 open for a raw read; each prints "neg NAME busy|NOT-BUSY ...".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
DEV=nvme0n1p7
LOOP=${LOOP:-120}
t0=$(date +%s)
refuse() {
	echo no | timeout 30 mkfs -t ufs --profile=native $DEV 2>&1
}
out=$(refuse)
reg=$(echo "$out" | sed -n 's/.*registration=\([0-9]*\).*/\1/p')
if [ -z "$reg" ]; then
	echo "first fail: $(echo "$out" | grep '^mkfs:' | head -1)"
else
	if echo "FORMAT $DEV:$reg" | timeout 600 mkfs -t ufs --profile=native $DEV > /dev/null 2>&1; then
		echo "first ok reg=$reg"
	else
		echo "first fail: FORMAT"
	fi
fi
ps ax 2> /dev/null | grep -v '\[' | head -40 | sed 's/^/ps: /'
mount | sed 's/^/mount: /'
load=
if [ "${LOAD:-0}" = 1 ]; then
	(while :; do dd if=/dev/zero of=/root/bug382.load bs=64k count=128 2> /dev/null; sync; done) &
	load=$!
	echo "load pid=$load"
fi
tries=0
fails=0
while [ $(($(date +%s) - t0)) -lt "$LOOP" ]; do
	out=$(refuse)
	tries=$((tries + 1))
	if echo "$out" | grep -q 'registration='; then
		echo "try t=$(($(date +%s) - t0)) ok"
	else
		fails=$((fails + 1))
		echo "try t=$(($(date +%s) - t0)) fail: $(echo "$out" | grep '^mkfs:' | head -1)"
	fi
	sleep 1
done
[ -n "$load" ] && kill "$load"
if [ "${NEG:-0}" = 1 ]; then
	(exec 3< /dev/$DEV; sleep 5) &
	sleep 1
	out=$(refuse)
	case "$out" in *busy*) echo "neg p7-open busy" ;; *) echo "neg p7-open NOT-BUSY: $(echo "$out" | head -1)" ;; esac
	wait
	(exec 3< /dev/nvme0n1; sleep 5) &
	sleep 1
	out=$(refuse)
	case "$out" in *busy*) echo "neg disk-open busy" ;; *) echo "neg disk-open NOT-BUSY: $(echo "$out" | head -1)" ;; esac
	wait
fi
echo "probe tries=$tries fails=$fails"
