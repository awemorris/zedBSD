#!/bin/sh
# The RTL8156 of the Latitude 5330 under zedBSD in Realtek's vendor mode (usb-rtl815x), in one run of
# plan/tools/hw5330/5330-run.sh from the NVMe: starts plan/ws221/tests/perf-peer.py on this host for two connections and
# has 5330-run.sh's EXTRA step run on the 5330's zedBSD:
#   the previous start's kernel log lines of the adapter (/var/log/kernel.log.old: a start that could not be reached);
#   lsusb -v (the USB devices and their drivers, for the other devices' non-regression);
#   the adapter's attach and link lines (usb-rtl815x, or usb-cdc-ncm with CFG_ADD=usb.rtl815x=off); ifconfig ue0;
#   tcp-perf send and recv against this host (SECONDS each, default 30), then both at once (a second peer on the
#   next port); ping 1472;
#   as root: the MTU changed while up (9000, then back to 1500) and ping 1472 again; ifconfig ue0 down and up, a wait
#   for the link, and ping 1472 again;
#   net.tcpstats' first line and ifconfig ue0's counters.
# 5330-run.sh then takes its survey, reboots to Debian and compares.  Output: build/ws315-hw/NAME (fresh-out.sh):
# summary.txt (the lines above), extra.txt (the whole EXTRA output), run.txt (5330-run.sh), peer.log.
#   plan/ws315/tests/5330-rtl.sh IMAGE NAME [SECONDS]
# MODE=--nvme-kernel (the default) makes IMAGE a kernel, put alone on the NVMe's zedBSD (the userland stays the one an
# image run wrote); MODE=--nvme-image writes IMAGE, an image of plan/ws315/tests/config-5330-rtl.mk.  CFG_ADD is passed
# to 5330-run.sh (for example CFG_ADD=usb.rtl815x=off for the CDC NCM A/B).  CAMERA=1 also streams ten frames of the
# built-in camera with camcap.  The 5330's Debian network settings are not touched.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
image=${1:?usage: 5330-rtl.sh IMAGE NAME [SECONDS]}
name=${2:?usage: 5330-rtl.sh IMAGE NAME [SECONDS]}
seconds=${3:-30}
server_ip=$(ip route get 10.0.30.3 | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -1)
port=5311
. plan/tools/fresh-out.sh
fresh_out "build/ws315-hw/$name"
out=build/ws315-hw/$name
say() { echo "5330-rtl: $*" | tee -a "$out/summary.txt"; }
say "image $image, peer $server_ip, ${seconds} s, cfg_add ${CFG_ADD:-none}"

: > "$out/peer.log"
timeout 1800 python3 -I plan/ws221/tests/perf-peer.py "$server_ip" "$port" "$out/peer.log" 3 > /dev/null 2>&1 &
peer=$!
timeout 1800 python3 -I plan/ws221/tests/perf-peer.py "$server_ip" $((port + 1)) "$out/peer.log" 1 > /dev/null 2>&1 &
peer2=$!

extra="echo old-start; echo user | sudo -S grep -E 'usb-rtl815x|usb-cdc-(ncm|ecm):|panic|trap' /var/log/kernel.log.old | tail -40; echo; echo ---; lsusb -v; echo ---; dmesg | grep -E 'usb-rtl815x|usb-cdc-(ncm|ecm):|usb: '; echo ---; ifconfig ue0; echo ---;"
# net.tcpstats is read 3 s before the end of the recv and of both (the receive buffer's size while it is in use).
sample="(sleep $((seconds - 3)); sysctl net.tcpstats)"
extra="$extra tcp-perf send $server_ip $port $seconds | tail -1; $sample > /tmp/ws315-stats-recv.txt 2>&1 &"
extra="$extra tcp-perf recv $server_ip $port $seconds | tail -1; wait; echo stats-recv; cat /tmp/ws315-stats-recv.txt; echo ---;"
extra="$extra echo both; tcp-perf send $server_ip $port $seconds > /tmp/ws315-both-send.txt 2>&1 &"
extra="$extra $sample > /tmp/ws315-stats-both.txt 2>&1 &"
extra="$extra tcp-perf recv $server_ip $((port + 1)) $seconds | tail -1; wait; tail -1 /tmp/ws315-both-send.txt;"
extra="$extra echo stats-both; cat /tmp/ws315-stats-both.txt; echo ---;"
extra="$extra echo ping-a; ping -c 3 -W 3 -s 1472 $server_ip; echo ---;"
extra="$extra echo user | sudo -S ifconfig ue0 mtu 9000; echo mtu9000-rc=\$?; ifconfig ue0 | head -1;"
extra="$extra echo user | sudo -S ifconfig ue0 mtu 1500; echo mtu1500-rc=\$?; echo ping-b; ping -c 3 -W 3 -s 1472 $server_ip; echo ---;"
# The down and up run detached on the 5330: this SSH session goes over ue0 itself and does not live through the down.
extra="$extra echo user | sudo -S sh -c '(ifconfig ue0 down; echo down-rc=\$?; sleep 2; ifconfig ue0 up; echo up-rc=\$?) > /tmp/ws315-cycle.txt 2>&1 &';"
extra="$extra sleep 15; cat /tmp/ws315-cycle.txt; echo ping-c; ping -c 3 -W 3 -s 1472 $server_ip; echo ---;"
extra="$extra dmesg | grep -E 'usb-rtl815x' | tail -6; echo ---; sysctl net.tcpstats | head -1; ifconfig ue0"
[ "${CAMERA:-0}" = 1 ] && extra="$extra; echo ---; echo user | sudo -S camcap -n 10 -t 5000 -o /tmp/ws315-cam.jpg > /tmp/ws315-cam.txt 2>&1; echo camcap-rc=\$?; tail -16 /tmp/ws315-cam.txt"
T=$((seconds * 3 + 180)) EXTRA=$extra timeout 2700 sh plan/tools/hw5330/5330-run.sh "${MODE:---nvme-kernel}" "$image" "ws315-$name" \
	> "$out/run.txt" 2>&1
status=$?
cp build/hw5330/logs/ws315-$name-extra.txt "$out/extra.txt" 2> /dev/null
kill "$peer" "$peer2" 2> /dev/null
grep -E '^5330-run: ' "$out/run.txt" | sed 's/^/5330-rtl: /' | tee -a "$out/summary.txt"
grep -E '^Bus |Driver ' "$out/extra.txt" | sed 's/^[[:space:]]*/5330-rtl: usb: /' | tee -a "$out/summary.txt"
sed -n '/^old-start/,/^---/p' "$out/extra.txt" | sed 's/^/5330-rtl: previous start: /' | tee -a "$out/summary.txt"
grep -E 'usb-rtl815x|usb-cdc-(ncm|ecm):|usb: ' "$out/extra.txt" | sed 's/^/5330-rtl: log: /' | tee -a "$out/summary.txt"
grep -E '^TCP-PERF|^both' "$out/extra.txt" | sed 's/^/5330-rtl: /' | tee -a "$out/summary.txt"
# The pool and each connection to this host's peers (receive window and buffer) from the two net.tcpstats readings.
for stats in recv both; do
	sed -n "/^stats-$stats/,/^---/p" "$out/extra.txt" | grep -E "packets_total=|remote=$server_ip:" |
	    sed -E 's/.*(packets_total=[0-9]+ packets_used=[0-9]+ packets_used_max=[0-9]+ packet_alloc_failures=[0-9]+).*/\1/;
		s/.*(remote=[^ ]+).*(srtt_us=[0-9]+).*(receive_window=[0-9]+).*(receive_bytes=[0-9]+).*(receive_limit_bytes=[0-9]+).*/\1 \2 \3 \4 \5/' |
	    sed "s/^/5330-rtl: stats-$stats: /" | tee -a "$out/summary.txt"
done
sed 's/^/5330-rtl: peer: /' "$out/peer.log" | tee -a "$out/summary.txt"
grep -E 'ping-[abc]|-rc=|packets transmitted|sendto|ifconfig:|mtu ' "$out/extra.txt" | sed 's/^/5330-rtl: /' | tee -a "$out/summary.txt"
grep -E 'RX packets|TX packets|errors' "$out/extra.txt" | tail -4 | sed 's/^/5330-rtl: after: /' | tee -a "$out/summary.txt"
sed -n '/^camcap-rc=/,$p' "$out/extra.txt" | sed 's/^/5330-rtl: camera: /' | tee -a "$out/summary.txt"
say "5330-run exit $status"
exit $status
