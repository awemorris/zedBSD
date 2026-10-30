#!/bin/sh
# The Latitude 5330's Debian (the RTL8156B under Linux's r8152) sending and receiving at once with zedBSD's tool, the
# baseline for the "both" line of plan/tools/rtl815x/5330-rtl.sh: builds userland/tests/tcp-perf for Linux on this host,
# copies it to the 5330 (ssh alias solaris10-man), starts plan/ws221/tests/perf-peer.py on this host on two ports,
# and runs tcp-perf send on the first and recv on the second at the same time, SECONDS each (default 30).
#   plan/tools/rtl815x/5330-linux-both.sh NAME [SECONDS]
# Output: build/rtl815x-linux/NAME (fresh-out.sh): summary.txt, client.txt (the 5330's output), peer.log.  The 5330 must
# be running Debian; nothing is changed there but the copied program (~/rtl815x/tcp-perf).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
name=${1:?usage: 5330-linux-both.sh NAME [SECONDS]}
seconds=${2:-30}
server_ip=$(ip route get 10.0.30.3 | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -1)
port=5321
. plan/tools/fresh-out.sh
fresh_out "build/rtl815x-linux/$name"
out=build/rtl815x-linux/$name
say() { echo "5330-linux-both: $*" | tee -a "$out/summary.txt"; }
lin() { timeout "${T:-60}" ssh -o ConnectTimeout=8 -o BatchMode=yes solaris10-man "$1"; }

lin 'uname -a' | grep -q Debian || { say "FAIL: the 5330 is not running Debian"; exit 1; }
cc -O2 -Wall -o "$out/tcp-perf" userland/tests/tcp-perf/main.c || { say "FAIL: build of tcp-perf"; exit 1; }
lin 'mkdir -p ~/rtl815x' && timeout 60 scp -q -o ConnectTimeout=8 -o BatchMode=yes "$out/tcp-perf" solaris10-man:rtl815x/tcp-perf ||
	{ say "FAIL: copy to the 5330"; exit 1; }

dev=$(lin "ip route get $server_ip" | sed -n 's/.* dev \([^ ]*\).*/\1/p' | head -1)
say "interface $dev driver $(lin "basename \$(readlink -f /sys/class/net/$dev/device/driver)") speed $(lin "cat /sys/class/net/$dev/speed") kernel $(lin 'uname -r')"

: > "$out/peer.log"
timeout $((seconds + 120)) python3 -I plan/ws221/tests/perf-peer.py "$server_ip" "$port" "$out/peer.log" 1 > /dev/null 2>&1 &
peer=$!
timeout $((seconds + 120)) python3 -I plan/ws221/tests/perf-peer.py "$server_ip" $((port + 1)) "$out/peer.log" 1 > /dev/null 2>&1 &
peer2=$!
sleep 1
T=$((seconds + 60)) lin "~/rtl815x/tcp-perf send $server_ip $port $seconds > /tmp/rtl815x-both-send.txt 2>&1 & ~/rtl815x/tcp-perf recv $server_ip $((port + 1)) $seconds | tail -1; wait; tail -1 /tmp/rtl815x-both-send.txt" \
	> "$out/client.txt" 2>&1
wait "$peer" "$peer2" 2> /dev/null
grep '^TCP-PERF' "$out/client.txt" | sed 's/^/5330-linux-both: /' | tee -a "$out/summary.txt"
sed 's/^/5330-linux-both: peer: /' "$out/peer.log" | tee -a "$out/summary.txt"
[ "$(grep -c '^TCP-PERF' "$out/client.txt")" = 2 ] || { say "FAIL"; exit 1; }
say "PASS"
