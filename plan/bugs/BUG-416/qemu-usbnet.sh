#!/bin/sh
# The guest's frames through QEMU's USB network adapter (qemu-xhci, usb-net, CDC-ECM, the guest's ue0), when a frame
# is a whole number of the bulk OUT endpoint's 64-byte packets and needs a zero-length packet after it.
#   sh plan/bugs/BUG-416/qemu-usbnet.sh BUILD [OUT]
# BUILD holds hdd-image.img, built with
#   plan/tools/guest/test-image.sh --no-harness plan/bugs/BUG-416/config-amd64.mk BUILD \
#       --file /etc/net.conf=plan/tools/guest/net.conf
# OUT (default build/p3-bug416/run, a fresh run directory) gets the pcap of the frames the adapter sent on, the host
# server's requests and the guest's answers.  The QEMU run is named p3-bug416; the disk is opened with snapshot=on.
# Over the serial console (plan/tools/guest/serial.py) the guest pings the host (10.0.2.2) with frames of 142, 640,
# 641, 1024 and 1472 bytes and fetches from the host's http-sink.py with request paths of 560..640 and 920..984
# bytes (every length modulo 64 twice, one TCP segment each: fetch takes paths up to 1023 bytes), each with a 10 s limit.  The verdict is the guest's own
# exit statuses (ping, fetch); pcap-sizes.py adds which whole-packet frames reached QEMU's network.
# The last line is qemu-usbnet: PASS or qemu-usbnet: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
PATH=/usr/bin:/bin:$PATH
export PATH
python=/usr/bin/python3
build=${1:?usage: qemu-usbnet.sh BUILD [OUT]}
out=${2:-build/p3-bug416/run}
. plan/tools/fresh-out.sh
fresh_out "$out"
mac=52:54:00:33:41:61
fail() {
	echo "qemu-usbnet: FAIL ($1)"
	exit 1
}
[ -f "$build/hdd-image.img" ] || fail "no $build/hdd-image.img"

# 1. The host's HTTP server on a free port.
port=$("$python" -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
timeout 1800 "$python" -I plan/bugs/BUG-416/http-sink.py "$port" "$out/requests.txt" > "$out/http-sink.txt" 2>&1 &
sink=$!

# 2. The guest: UEFI, NVMe (snapshot), the serial console and QMP on unix sockets, the user network on usb-net.
. plan/tools/guest/qemu-accel.sh
ovmf_code=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
cp "${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}" "$out/vars.fd"
socket=$top/$out/serial.sock
timeout 1800 qemu-system-x86_64 -name p3-bug416 -machine q35 -m 4G -smp 4 $(qemu_accel_args max) \
    -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
    -drive "if=pflash,format=raw,file=$out/vars.fd" \
    -drive "if=none,id=boot,file=$build/hdd-image.img,format=raw,snapshot=on" \
    -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
    -device qemu-xhci,id=xhci \
    -netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dhcpstart=10.0.2.15,dns=10.0.2.3 \
    -object "filter-dump,id=dump0,netdev=net0,file=$top/$out/net.pcap" \
    -device usb-net,bus=xhci.0,port=1,id=ecm,netdev=net0,mac=$mac,msos-desc=on \
    -vga std -display none -no-reboot \
    -qmp "unix:$top/$out/qmp.sock,server=on,wait=off" \
    -serial "unix:$socket,server=on,wait=off" -monitor none > "$out/qemu.txt" 2>&1 &
qemu=$!
trap 'kill "$qemu" "$sink" 2> /dev/null; wait "$qemu" "$sink" 2> /dev/null; sync' EXIT
tries=0
while [ ! -S "$socket" ] && [ $tries -lt 600 ]; do
	kill -0 "$qemu" 2> /dev/null || fail "QEMU ended, $out/qemu.txt"
	sleep 0.1
	tries=$((tries + 1))
done
serial="$python -I plan/tools/guest/serial.py --socket $socket --timeout 240"
$serial expect 'login: ' > /dev/null || fail "no login prompt"
$serial login > /dev/null || fail "login"
echo "qemu-usbnet: logged in on the serial console"

# 3. The network: ue0 has its DHCP address.
tries=0
while [ $tries -lt 30 ]; do
	if $serial run 'ifconfig ue0' > "$out/ifconfig.txt" 2>&1 && grep -q 'inet 10\.0\.2\.' "$out/ifconfig.txt"; then
		break
	fi
	sleep 2
	tries=$((tries + 1))
done
grep -q 'inet 10\.0\.2\.' "$out/ifconfig.txt" || fail "no DHCP address on ue0, $out/ifconfig.txt"
echo "qemu-usbnet: ue0 $(grep -o 'inet 10\.0\.2\.[0-9]*' "$out/ifconfig.txt")"

# 4. Pings: an ICMP echo of N bytes is a frame of N + 42.
ping_bad=0
for size in 100 598 599 982 1430; do
	if $serial run "ping -c 3 -W 3 -s $size 10.0.2.2" > "$out/ping-$size.txt" 2>&1; then
		echo "qemu-usbnet: ping frame $((size + 42)): ok"
	else
		echo "qemu-usbnet: ping frame $((size + 42)): FAILED ($out/ping-$size.txt)"
		ping_bad=$((ping_bad + 1))
	fi
done

# 5. Fetches with request paths of every length modulo 64, twice.
loop="for L in \$(seq 560 640) \$(seq 920 984); do p=\$(printf '%0'\$L'd' 0); \
if timeout 10 fetch -q -o /dev/null \"http://10.0.2.2:$port/\$p\" > /dev/null 2>&1; then echo \"fetch ok \$L\"; \
else echo \"fetch failed \$L\"; fi; done"
"$python" -I plan/tools/guest/serial.py --socket "$socket" --timeout 1700 run "$loop" > "$out/fetch.txt" 2>&1
fetch_ok=$(grep -c '^fetch ok' "$out/fetch.txt")
fetch_bad=$(grep -c '^fetch failed' "$out/fetch.txt")
echo "qemu-usbnet: fetch $fetch_ok ok, $fetch_bad failed of 146$(grep '^fetch failed' "$out/fetch.txt" | \
    sed 's/^fetch failed /, /' | tr -d '\n')"
echo "qemu-usbnet: the host server saw $(grep -c '^request' "$out/requests.txt" 2> /dev/null || echo 0) requests"

# 6. The frames that reached QEMU's network.
"$python" -I plan/bugs/BUG-416/pcap-sizes.py "$out/net.pcap" "$mac"

# The verdict, from the guest's own exit statuses.
if [ "$ping_bad" -eq 0 ] && [ "$fetch_ok" -eq 146 ] && [ "$fetch_bad" -eq 0 ]; then
	echo "qemu-usbnet: PASS"
	exit 0
fi
echo "qemu-usbnet: FAIL"
exit 1
