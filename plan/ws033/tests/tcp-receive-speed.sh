#!/bin/sh
# q786 (BUG-222): how fast the guest's TCP takes a file, on the SSH guest (plan/tools/guest/guest.py; image:
# plan/tools/guest/test-image.sh plan/tools/guest/config-amd64-ssh.mk BUILD, built from the commit under test).
# The host serves a file of SIZE MiB (default 32) over HTTP on PORT (default 18786) of 127.0.0.1, which the guest
# reaches through QEMU's user network as 10.0.2.2; the guest fetches it RUNS times (default 3) into /tmp and checks
# its cksum.  Judged over SSH only, never the console.
#  1. Every run's cksum equals the host's (no corrupted or short transfer).
#  2. The seconds of each run and the MB/s (host-timed around the fetch, less the time of an empty SSH command)
#     are printed for the comparison with another commit; there is no fixed threshold (QEMU's user network
#     and the guest's CDC ECM adapter bound it, not only the TCP window).
#   plan/tools/guest/guest.py start IMAGE; plan/tools/guest/guest.py wait
#   plan/ws033/tests/tcp-receive-speed.sh [OUTDIR] [SIZE] [RUNS] [PORT]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws033-tcp-receive-speed}
size=${2:-32}
runs=${3:-3}
port=${4:-18786}
mkdir -p "$out/www"
guest() { timeout 300 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
status=0

# The file, and its cksum as the guest's cksum prints it (POSIX cksum: CRC and size).
head -c $((size * 1048576)) /dev/urandom > "$out/www/blob"
expected=$(cksum < "$out/www/blob" | awk '{ print $1, $2 }')
echo "file: $size MiB, cksum $expected"

# The server, stopped on the way out.
python3 -m http.server "$port" --bind 127.0.0.1 --directory "$out/www" > "$out/http.log" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null' EXIT
sleep 1

# The cost of an SSH command that does nothing, taken off each run.
start=$(date +%s.%N)
guest "true" > /dev/null
end=$(date +%s.%N)
empty=$(echo "$end - $start" | bc)
echo "empty ssh command: $empty s"

# The adapter's counters before the runs, to compare with a run that fails (T1-513: one run in several, the fetch and
# its SSH session both stopped for about 120 s, "Connection to 127.0.0.1 closed by remote host", and the next run went
# through).  RX errors that grow over the failed run are frames the driver could not take (no packet buffer, or a
# failed transfer); for the packets themselves start the guest with QEMU's dump of its network
# (guest.py start IMAGE --qemu-extra "-object filter-dump,id=dump0,netdev=net0,file=OUTDIR/net0.pcap").
guest "ifconfig -a" > "$out/before.txt"

# The runs.
i=1
while [ $i -le "$runs" ]; do
	guest "rm -f /tmp/blob" > /dev/null
	start=$(date +%s.%N)
	guest "fetch -o /tmp/blob http://10.0.2.2:$port/blob" > "$out/run$i-fetch.txt"
	end=$(date +%s.%N)
	got=$(guest "cksum < /tmp/blob" | awk 'NF >= 2 { print $1, $2 }' | tail -1)
	seconds=$(echo "$end - $start - $empty" | bc)
	rate=$(echo "scale=2; $size * 1.048576 / $seconds" | bc 2>/dev/null)
	if [ "$got" = "$expected" ]; then
		echo "run $i: ok, $seconds s, ${rate:-?} MB/s"
	else
		echo "run $i: FAILED, cksum '$got' (expected '$expected'), $seconds s"
		guest "ifconfig -a" > "$out/run$i-guest.txt"
		echo "  ue0 RX before: $(awk '/^ue0:/ {f = 1} f && /RX packets/ {print; exit}' "$out/before.txt" | tr -s ' ')"
		echo "  ue0 RX after:  $(awk '/^ue0:/ {f = 1} f && /RX packets/ {print; exit}' "$out/run$i-guest.txt" | tr -s ' ')"
		status=1
	fi
	i=$((i + 1))
done
guest "rm -f /tmp/blob; netstat 2>/dev/null | head -20" > "$out/after.txt"

echo "tcp-receive-speed: status $status (outputs in $out)"
exit $status
