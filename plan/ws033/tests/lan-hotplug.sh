#!/bin/sh
# ws033-p001 / BUG-168: a USB network adapter plugged in after the boot on the SSH guest (plan/tools/guest/guest.py,
# its management adapter is ue0; image: plan/tools/guest/test-image.sh plan/tools/guest/config-amd64-ssh.mk BUILD).
# The adapter is QEMU's usb-net (CDC ECM) on its own user network 10.0.5.0/24, plugged in through QMP on a port QEMU
# chooses (the controller has 8+8 ports).  Judged over SSH only (ifconfig, net show, netstat), never the console.
#  1. Plugged in: a new interface appears with the adapter's MAC, and within 40 s networkd brings it up and DHCP gives
#     it a 10.0.5.x address (BUG-168: it stayed down, or fell back to 169.254 with RX packets 0).
#  2. Its counters move (RX packets > 0): the data interface works after a late attach.  fetch over it (L1): the guest
#     fetches a 1 MiB file from a server the test starts on the host's loopback, through the adapter's network
#     (QEMU's user network takes 10.0.5.2 to the host), and its cksum is the host's.
#  3. Pulled out: the interface goes, the guest still answers over SSH (tried for 60 s; T1-140), networkd stays up;
#     plugged in again: an address again.
# Each poll's ifconfig, net show and the kernel's usb/net lines go to OUTDIR for the analysis when a step fails.
#   plan/tools/guest/guest.py start IMAGE; plan/tools/guest/guest.py wait
#   plan/ws033/tests/lan-hotplug.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
runtime=${GUEST_RUNTIME:-$PWD/build/guest}
qmp="$runtime/qmp.sock"
out=${1:-build/ws033-lan-hotplug}
mkdir -p "$out"
guest() { timeout 60 python3 plan/tools/guest/guest.py run "$1" 2>&1; }
send() { timeout 40 python3 plan/ws049/tests/qmp-send.py "$qmp" "$@" >> "$out/qmp.txt" 2>&1; }
mac=52:54:00:33:00:05
server_pid=
status=0
pass() { echo "$1: ok"; }
fail() { echo "$1: FAILED"; status=1; }
: > "$out/qmp.txt"

# The interface with the adapter's MAC, or nothing.
hot_name() { guest "ifconfig -a" | awk -v mac="$mac" '/^[a-z]+[0-9]+:/ { name = $1; sub(":", "", name) } tolower($0) ~ mac { print name; exit }'; }

# Polls up to 40 s for a 10.0.5.x address on the adapter; each poll is kept.  Prints the interface's name.
wait_address() {
	tag=$1
	i=0
	while [ $i -lt 20 ]; do
		name=$(hot_name)
		if [ -n "$name" ]; then
			guest "ifconfig $name; net show; route show" > "$out/$tag-poll$i.txt"
			if grep -q 'inet 10\.0\.5\.' "$out/$tag-poll$i.txt"; then
				echo "$name"
				return 0
			fi
		fi
		sleep 2
		i=$((i + 1))
	done
	echo "${name:-}"
	return 1
}

# A file and an HTTP server on the host's loopback, for fetch (stopped at the end).
mkdir -p "$out/www"
head -c 1048576 /dev/urandom > "$out/www/hot.bin"
port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
python3 -m http.server "$port" --bind 127.0.0.1 --directory "$out/www" > "$out/http.txt" 2>&1 &
server_pid=$!
trap '[ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null' EXIT

# The before state.
guest "ifconfig -a; net show; dmesg | tail -40" > "$out/before.txt"

# 1 and 2. Plugged in.
send netdev_add '{"type":"user","id":"hotnet","net":"10.0.5.0/24","host":"10.0.5.2","dhcpstart":"10.0.5.15"}'
send device_add "{\"driver\":\"usb-net\",\"bus\":\"xhci.0\",\"netdev\":\"hotnet\",\"id\":\"hotnic\",\"mac\":\"$mac\"}"
name=$(wait_address plug1)
got=$?
guest "dmesg | grep -Ei 'usb|cdc|ue[0-9]' | tail -30; ps -A -o pid,args | grep [n]etworkd" > "$out/plug1-kernel.txt"
echo "interface: ${name:-none}"
[ -n "$name" ] && pass plugged-interface || fail plugged-interface
[ $got -eq 0 ] && pass plugged-dhcp-address || fail plugged-dhcp-address
if [ -n "$name" ]; then
	guest "ifconfig $name" > "$out/plug1-counters.txt"
	rx=$(sed -n 's/.*RX packets \([0-9]*\).*/\1/p' "$out/plug1-counters.txt" | head -1)
	echo "RX packets: ${rx:-?}"
	[ "${rx:-0}" -gt 0 ] 2>/dev/null && pass plugged-rx || fail plugged-rx
	# A failed address: what a manual DHCP gets, for the analysis (not judged).
	[ $got -eq 0 ] || guest "net dhcp $name --timeout=20; ifconfig $name" > "$out/plug1-manual-dhcp.txt"
fi

# fetch through the plugged adapter's network.
want=$(cksum < "$out/www/hot.bin" | awk '{print $1, $2}')
guest "fetch -q -o /tmp/hot.bin http://10.0.5.2:$port/hot.bin; echo exit=\$?; cksum < /tmp/hot.bin" > "$out/plug1-fetch.txt"
cat "$out/plug1-fetch.txt"
have=$(grep -v '^exit=' "$out/plug1-fetch.txt" | awk 'NF >= 2 {print $1, $2}' | tail -1)
echo "cksum: host $want, guest ${have:-none}"
grep -q '^exit=0' "$out/plug1-fetch.txt" && [ "$have" = "$want" ] && pass plugged-fetch || fail plugged-fetch

# 3. Pulled out, networkd stays; plugged in again.  The routes before the pull are kept (T1-140: once the guest's SSH,
# which goes through the harness's adapter ue0, stopped answering after the pull).
guest "route show; net show" > "$out/before-unplug-routes.txt"
send device_del '{"id":"hotnic"}'
sleep 4
left=$(hot_name)
[ -z "$left" ] && pass unplugged-gone || fail unplugged-gone

# SSH after the pull, tried for 60 s: a short loss and a guest that stopped are told apart (each try's time is kept).
i=0
answered=0
: > "$out/after-unplug-ssh.txt"
while [ $i -lt 12 ]; do
	reply=$(guest 'echo alive; route show; net show; ps -A -o args | grep -c "[n]etworkd"')
	echo "try $i at $(date +%s): $reply" >> "$out/after-unplug-ssh.txt"
	if printf '%s\n' "$reply" | grep -q '^alive$'; then
		answered=1
		break
	fi
	sleep 5
	i=$((i + 1))
done
echo "ssh after the pull: answered=$answered after $i failed tries"
[ $answered -eq 1 ] && pass ssh-after-unplug || fail ssh-after-unplug
printf '%s\n' "$reply" | tail -1 > "$out/networkd-count.txt"
[ "$(cat "$out/networkd-count.txt")" = "1" ] && pass networkd-alive || fail networkd-alive
send device_add "{\"driver\":\"usb-net\",\"bus\":\"xhci.0\",\"netdev\":\"hotnet\",\"id\":\"hotnic\",\"mac\":\"$mac\"}"
name=$(wait_address plug2)
[ $? -eq 0 ] && pass replugged-dhcp-address || fail replugged-dhcp-address
send device_del '{"id":"hotnic"}'
send netdev_del '{"id":"hotnet"}'

echo "lan-hotplug: status $status (outputs in $out)"
exit $status
