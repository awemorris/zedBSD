#!/bin/sh
# ws333 p025: whether wakeups like zedBSD's periodic tick keep the 5330's package out of PC2 (zedBSD: every core
# about 88 % in C6, PC2 0 %; Debian, which has no graphics driver loaded on this machine: PC2 about 99 %).  Debian
# only, under /tmp/i915-hw.lock: 30 s of idle, then 30 s with every CPU woken on a common period of 1 ms and of 4 ms
# (waker-debian.py), each with the package power, PC2..PC10 and CPU 0's core states (msr-debian.py idle).
#   sh plan/ws333/tests/hw5330/5330-pc2-debian.sh NAME        (summary: build/hw5330/ws333/NAME/summary.txt)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
NAME=${1:?NAME}
cd "$(dirname "$0")/../../../.."
LOGS=build/hw5330/ws333/$NAME
HOST=solaris10-man
mkdir -p "$LOGS"
SUMMARY=$LOGS/summary.txt
: > "$SUMMARY"
say() {
	echo "5330-pc2: $*" | tee -a "$SUMMARY"
}
lin() {
	timeout "${TO:-120}" ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$HOST" "$1"
}
TO=60 timeout 60 scp -q plan/ws333/tests/hw5330/msr-debian.py plan/ws333/tests/hw5330/waker-debian.py "$HOST:ws245/" ||
	{ echo "5330-pc2: FAIL: copy of the helpers"; exit 1; }
exec 9> /tmp/i915-hw.lock
flock 9
TO=15 lin hostname | grep -q chaos || { say "FAIL: Debian is not up"; exit 1; }
lin 'sudo -n modprobe msr' > /dev/null 2>&1 || { say "FAIL: modprobe msr"; exit 1; }
say "$NAME start $(date '+%F %T') $(TO=15 lin 'uname -r; lsmod | grep -c -E "^(i915|xe) "' | tr '\n' ' ')(graphics drivers loaded)"
sleep 30
for period in idle 1000 4000; do
	if [ "$period" != idle ]; then
		TO=60 lin "python3 -I \$HOME/ws245/waker-debian.py $period 40" > /dev/null 2>&1 &
		sleep 5
	fi
	TO=60 lin 'sudo -n python3 -I $HOME/ws245/msr-debian.py idle 30' > "$LOGS/pc2-$period.txt" 2>&1
	wait
	say "$period: $(tr '\n' ';' < "$LOGS/pc2-$period.txt")"
	sleep 10
done
say "done $(date '+%F %T')"
