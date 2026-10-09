#!/bin/sh
# BUG-222: a TCP download into the zedBSD guest over a lossy link, the guest's receiving side being what is measured
# (its window, its answer to a gap: the duplicate acknowledgements and the segments kept ahead of the gap).  The guest is
# the SSH guest (plan/tools/guest/test-image.sh plan/tools/guest/config-amd64-ssh.mk BUILD) with a second USB network
# adapter (QEMU's usb-net, CDC ECM) on a host tap, on a bridge with a veth into the network namespace zbl, where a
# Python HTTP server serves a 32 MiB file at 10.77.0.1 and netem on the namespace's end drops a share of the packets
# going to the guest (with 1 ms of delay, a LAN's).  The guest's adapter is given 10.77.0.2 by hand.
#  For each loss (0 %, 0.5 %, 2 %): fetch the file three times; the cksum is the host's each time; the MB/s of each
#  (the host's clock around the guest's fetch, the SSH call included) and their median.
# PASS: every cksum, and the median at 2 % loss at least a quarter of the median at 0 % (a receiver that answers a
# gap only by the sender's timer falls to a few percent of it).  The numbers are reported for the comparison with the
# image before the change.  Judged over SSH, never the console.  The host's part needs sudo (ip, tc, the server).
#   plan/ws033/tests/tcp-loss-speed.sh IMAGE [OUTDIR]
# Each run gets a new directory behind OUTDIR (plan/tools/fresh-out.sh); nothing on the host is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
image=${1:?usage: tcp-loss-speed.sh IMAGE [OUTDIR]}
out=${2:-build/bug222-loss}
. plan/tools/fresh-out.sh
fresh_out "$out"
GUEST_RUNTIME=$(pwd)/$out/run
export GUEST_RUNTIME
guest() { timeout 300 python3 plan/tools/guest/guest.py run "$1" 2>&1 | tr -d '\r'; }
status=0
server_pid=

# The host's segment taken down: the server, the guest, the links and the namespace.
teardown() {
	python3 plan/tools/guest/guest.py stop >/dev/null 2>&1
	[ -n "$server_pid" ] && sudo kill "$server_pid" 2>/dev/null
	sudo ip link del zbltap0 2>/dev/null
	sudo ip link del zblbr0 2>/dev/null
	sudo ip netns del zbl 2>/dev/null
}
trap teardown EXIT

# The file, its cksum, and the segment: a bridge with the tap and a veth into the namespace, 10.77.0.1 there.
mkdir -p "$out/www"
head -c 33554432 /dev/urandom > "$out/www/f.bin"
want=$(cksum < "$out/www/f.bin" | awk '{print $1, $2}')
teardown
sudo ip netns add zbl || exit 1
sudo ip link add zblbr0 type bridge
sudo ip link set zblbr0 up
sudo ip tuntap add dev zbltap0 mode tap user "$(id -un)"
sudo ip link set zbltap0 master zblbr0 up
sudo ip link add zblv0 type veth peer name zblv1
sudo ip link set zblv0 master zblbr0 up
sudo ip link set zblv1 netns zbl
sudo ip -n zbl link set lo up
sudo ip -n zbl link set zblv1 up
sudo ip -n zbl addr add 10.77.0.1/24 dev zblv1
sudo ip netns exec zbl python3 -m http.server 8080 --bind 10.77.0.1 --directory "$out/www" > "$out/http.txt" 2>&1 &
server_pid=$!

# The guest with the second adapter on the tap, given 10.77.0.2.
python3 plan/tools/guest/guest.py start "$image" --qemu-extra "-netdev tap,id=net1,ifname=zbltap0,script=no,downscript=no \
-device usb-net,bus=xhci.0,port=5,id=ecm1,netdev=net1,mac=52:54:00:33:00:07" || exit 1
python3 plan/tools/guest/guest.py wait --timeout 240 || { echo "FAIL: the guest did not come up"; exit 1; }
iface=$(guest "ifconfig -a | awk '/^[a-z]+[0-9]+:/ {name = substr(\$1, 1, length(\$1) - 1)} /ether 52:54:00:33:00:07/ {print name}'" |
    tail -1)
echo "interface: $iface"
[ -n "$iface" ] || { echo "FAIL: no second adapter"; exit 1; }
# The address through networkd (net static, so that its DHCP on the hot-plugged adapter does not take it back), with
# ifconfig's two steps when that is refused: zedBSD's ifconfig takes "inet ADDRESS netmask MASK" and "up" as separate
# commands (T1-484: the one line "inet ... netmask ... up" printed the usage and left the adapter without an address).
guest "net static $iface ipv4 10.77.0.2 netmask 255.255.255.0 || ifconfig $iface inet 10.77.0.2 netmask 255.255.255.0; ifconfig $iface up; sleep 2; ifconfig $iface; ping -c 3 10.77.0.1" > "$out/setup.txt"
grep -qE ' [1-3] packets received' "$out/setup.txt" || { cat "$out/setup.txt"; echo "FAIL: no way to 10.77.0.1"; exit 1; }

# A TCP probe before the measurement (T1-511: ping answered, but every fetch failed with "cannot connect" after about
# 30 s and the server saw no request): a small file fetched while the packets are captured on both ends of the segment,
# the tap (what QEMU sends and gets) and the namespace's veth (what the server sends and gets).  When it fails, the
# captures, the guest's interfaces and routes and the namespace's sockets and neighbours are kept, and the measurement
# is not run (nine fetches that each wait out the connect would tell nothing more).
head -c 65536 /dev/urandom > "$out/www/small.bin"
sudo dumpcap -q -i zbltap0 -a duration:25 -w - > "$out/probe-tap.pcapng" 2> "$out/probe-tap.err" &
tap_capture=$!
sudo ip netns exec zbl dumpcap -q -i zblv1 -a duration:25 -w - > "$out/probe-veth.pcapng" 2> "$out/probe-veth.err" &
veth_capture=$!
sleep 3
guest "timeout 15 fetch -q -o /tmp/small.bin http://10.77.0.1:8080/small.bin; echo exit=\$?; ls -l /tmp/small.bin" > "$out/probe.txt"
guest "ifconfig -a; route show" > "$out/probe-guest.txt"
{ sudo ip netns exec zbl ss -tan; sudo ip netns exec zbl ip neigh; sudo ip netns exec zbl ip -s link show zblv1; } > "$out/probe-namespace.txt" 2>&1
wait "$tap_capture" "$veth_capture"
tshark -r "$out/probe-tap.pcapng" -n > "$out/probe-tap.txt" 2>&1
tshark -r "$out/probe-veth.pcapng" -n > "$out/probe-veth.txt" 2>&1
if ! grep -q '^exit=0' "$out/probe.txt"; then
	cat "$out/probe.txt"
	echo "tap (QEMU's side):"
	grep -E 'TCP|ARP' "$out/probe-tap.txt" | head -12
	echo "veth (the server's side):"
	grep -E 'TCP|ARP' "$out/probe-veth.txt" | head -12
	echo "FAIL: no TCP connection to 10.77.0.1:8080 (probe-*.txt in $out)"
	echo "tcp-loss-speed: FAIL"
	exit 1
fi
echo "probe: ok"

# One loss: three fetches, their MB/s and the median.
measure() {
	loss=$1
	sudo ip netns exec zbl tc qdisc replace dev zblv1 root netem delay 1ms loss "$loss%"
	: > "$out/rates-$loss.txt"
	run=1
	while [ $run -le 3 ]; do
		start=$(date +%s%N)
		guest "fetch -q -o /tmp/f.bin http://10.77.0.1:8080/f.bin; echo exit=\$?; cksum < /tmp/f.bin" > "$out/fetch-$loss-$run.txt"
		end=$(date +%s%N)
		have=$(grep -v '^exit=' "$out/fetch-$loss-$run.txt" | awk 'NF >= 2 {print $1, $2}' | tail -1)
		rate=$(awk -v ns=$((end - start)) 'BEGIN { printf "%.2f", 33554432 / 1048576 / (ns / 1e9) }')
		echo "loss $loss% run $run: $rate MB/s cksum ${have:-none}"
		echo "$rate" >> "$out/rates-$loss.txt"
		if ! grep -q '^exit=0' "$out/fetch-$loss-$run.txt" || [ "$have" != "$want" ]; then
			echo "FAIL: loss $loss% run $run: the file differs or fetch failed"
			echo "loss $loss% run $run" >> "$out/failed.txt"
		fi
		run=$((run + 1))
	done
	sort -n "$out/rates-$loss.txt" | sed -n 2p
}
median0=$(measure 0 | tee /dev/stderr | tail -1)
measure 0.5 | tee /dev/stderr > "$out/m05.txt"
median2=$(measure 2 | tee /dev/stderr | tail -1)
[ -s "$out/failed.txt" ] && status=1
echo "median MB/s: loss 0% $median0, loss 0.5% $(tail -1 "$out/m05.txt"), loss 2% $median2"
awk -v a="$median0" -v b="$median2" 'BEGIN { exit !(b >= a / 4) }' &&
    echo "ok: at 2% loss at least a quarter of the speed without loss" ||
    { echo "FAIL: at 2% loss $median2 MB/s, below a quarter of $median0"; status=1; }

[ $status = 0 ] && echo "tcp-loss-speed: PASS" || echo "tcp-loss-speed: FAIL"
exit $status
