#!/bin/sh
# The time of one SSH round trip to a QEMU guest ("true" over a new connection), ROUNDS times (default 15), with
# the guest's frames written to a pcap by the emulator (filter-dump on the user network, the guest's side of it).
# Each round's host-side time is printed with what the capture shows for its connection (the SYN-ACK, the banners,
# the FINs as offsets from the SYN, the longest the guest took to answer and what it answered, retransmissions) and
# the boot disk's reads, writes and flushes in it (QMP query-blockstats).  net.tcpstats is read before and after,
# and the logins counted in /var/log/messages.  A round passes under LIMIT seconds (default 1).
# WRITE_IOPS=N limits the guest disk's writes to N a second (QMP block_set_io_throttle), the way a host busy writing
# slows the emulator's disk; LOAD=N runs N busy loops on the host (nice 10) during the rounds; CPUS sets the guest's
# CPUs (default 4); LOGCHECK=1 then runs plan/tools/ssh-rtt/log-check.sh in the guest (no line lost, a stopped
# syslogd does not hold a sender).  Judged over SSH and the capture, never the console.
#   plan/tools/ssh-rtt/qemu-ssh-rtt.sh IMAGE [OUT]
# The guest runs in OUT/run (default build/ssh-rtt).  The last line is "qemu-ssh-rtt: PASS" or "qemu-ssh-rtt: FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
image=${1:?usage: qemu-ssh-rtt.sh IMAGE [OUT]}
out=${2:-build/ssh-rtt}
mkdir -p "$out"
case $out in
/*) ;;
*) out=$(pwd)/$out ;;
esac
GUEST_RUNTIME=$out/run
export GUEST_RUNTIME
guest() { timeout "${T:-120}" python3 plan/tools/guest/guest.py "$@"; }
burners=
trap 'guest stop > /dev/null 2>&1; [ -n "$burners" ] && kill $burners 2> /dev/null' EXIT
pcap=$out/net.pcap
guest start --cpus "${CPUS:-4}" --qemu-extra "-object filter-dump,id=dump0,netdev=net0,file=$pcap" "$image" > "$out/start.txt" ||
	{ echo "qemu-ssh-rtt: FAIL (start)"; exit 1; }
T=300 guest wait --timeout 240 > /dev/null || { echo "qemu-ssh-rtt: FAIL (no SSH)"; exit 1; }
port=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["ssh_port"])' "$GUEST_RUNTIME/session.json")
guest run 'sysctl net.tcpstats' > "$out/tcpstats-before.txt" 2>&1
: > "$out/rounds.txt"
load=${LOAD:-0}
if [ "$load" -gt 0 ]; then
	for i in $(seq "$load"); do
		nice -n 10 timeout 900 sh -c 'while :; do :; done' &
		burners="$burners $!"
	done
fi
monitor=$GUEST_RUNTIME/qmp.sock
# The boot disk's counts (reads, writes, flushes and the emulator's time in each, in ms), from the emulator.
disk() {
	timeout 10 python3 plan/tools/qmp.py "$monitor" query-blockstats 2> /dev/null | python3 -c '
import json, sys
for device in json.load(sys.stdin)["return"]:
	if device.get("device") == "boot" or device.get("qdev", "").endswith("rootstick"):
		s = device["stats"]
		print(s["rd_operations"], s["wr_operations"], s["flush_operations"],
		      s["rd_total_time_ns"] // 1000000, s["wr_total_time_ns"] // 1000000, s["flush_total_time_ns"] // 1000000)
		break
else:
	print("- - - - - -")'
}
# WRITE_IOPS=N limits the boot disk's writes to N a second, the way a host busy writing slows the emulator's.
if [ -n "${WRITE_IOPS:-}" ]; then
	timeout 10 python3 plan/tools/qmp.py "$monitor" block_set_io_throttle \
		"{\"device\":\"boot\",\"bps\":0,\"bps_rd\":0,\"bps_wr\":0,\"iops\":0,\"iops_rd\":0,\"iops_wr\":$WRITE_IOPS}" > "$out/throttle.txt" 2>&1
fi
for round in $(seq "${ROUNDS:-15}"); do
	before=$(disk)
	begin=$(date +%s.%N)
	T=60 guest run true > /dev/null 2>&1
	status=$?
	end=$(date +%s.%N)
	echo "$round $begin $end $status $before $(disk)" >> "$out/rounds.txt"
done
[ -n "$burners" ] && kill $burners 2> /dev/null
burners=
# LOGCHECK=1 then checks that syslog keeps every line and does not hold a sender (plan/tools/ssh-rtt/log-check.sh).
logcheck=
if [ "${LOGCHECK:-0}" = 1 ]; then
	guest put plan/tools/ssh-rtt/log-check.sh /tmp/log-check.sh > /dev/null 2>&1
	logcheck=$(T=240 guest run 'sh /tmp/log-check.sh' 2>&1 | tr -d '\r')
fi
guest run 'sysctl net.tcpstats' > "$out/tcpstats-after.txt" 2>&1
# The log: every login is logged ("Accepted publickey"), and a line says so when messages were dropped.
# On a slow disk the log is behind; the count waits a little for the last logins (up to 30 s).
counts=$(guest run 'n=0; while [ $n -lt 30 ] && [ $(grep -c "Accepted publickey" /var/log/messages) -lt '"$((${ROUNDS:-15} + 4 + ${LOGCHECK:-0} * 2))"' ]; do sleep 1; n=$((n + 1)); done; echo $(grep -c "Accepted publickey" /var/log/messages) $(grep -c dropped /var/log/messages)' 2> /dev/null | tr -d '\r')
accepted=${counts% *}
dropped=${counts#* }
guest stop > /dev/null 2>&1
trap - EXIT
[ -n "$logcheck" ] && echo "$logcheck"
# The logins before the count: wait, the first reading, the rounds, the log check's two, the last reading, the count.
after=2
expected=$((${ROUNDS:-15} + 4))
if [ -n "$logcheck" ]; then
	after=4
	expected=$((expected + 2))
fi
echo "log: ${accepted:-?} logins accepted in /var/log/messages (expected $expected), ${dropped:-?} lines about dropped messages"
python3 -I plan/tools/ssh-rtt/rtt-summary.py "$pcap" "$out/rounds.txt" "${LIMIT:-1}" \
	"$out/tcpstats-before.txt" "$out/tcpstats-after.txt" "$after"
