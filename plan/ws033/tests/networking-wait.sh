#!/bin/sh
# ws033-p001 (L3): the boot's networking service with rc.conf's networking.wait true, on the SSH guest
# (plan/tools/guest/guest.py; its adapter ue0 is QEMU's usb-net on the user network, id "ecm").
# Image: plan/tools/guest/test-image.sh plan/tools/guest/config-amd64-ssh.mk BUILD \
#            --file /etc/rc.conf=plan/ws033/tests/rc-networking-wait.conf
#  A. With the adapter: the service ends when ue0 has its DHCP address: "service status networking" is completed,
#     and the guest answers over SSH.
#  B. Without it: the adapter is taken out through QMP as QEMU starts (before the guest boots), so no interface gets an
#     address; the service gives up after its 30 seconds (net startup's NET_STARTUP_WAIT_SECONDS) and the boot goes on.
#     After 75 seconds the adapter is plugged in again (late plug, ws033-p001 L1), networkd gives it its address and
#     SSH answers: "service status networking" is not completed (failed: no address in time) and sshd, which comes
#     after it, runs.
# Judged over SSH only (service status, ifconfig), never the console.  PASS: the last line networking-wait: PASS.
#   plan/ws033/tests/networking-wait.sh IMAGE [OUTDIR]
# Each run gets a new directory behind OUTDIR (plan/tools/fresh-out.sh); nothing on the host is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
image=${1:?usage: networking-wait.sh IMAGE [OUTDIR]}
out=${2:-build/ws033-networking-wait}
. plan/tools/fresh-out.sh
fresh_out "$out"
GUEST_RUNTIME=$(pwd)/$out/run
export GUEST_RUNTIME
qmp="$GUEST_RUNTIME/qmp.sock"
guest() { timeout 60 python3 plan/tools/guest/guest.py run "$1" 2>&1 | tr -d '\r'; }
send() { timeout 20 python3 plan/ws049/tests/qmp-send.py "$qmp" "$@" >> "$out/qmp.txt" 2>&1; }
status=0
pass() { echo "$1: ok"; }
fail() { echo "$1: FAILED"; status=1; }
trap 'python3 plan/tools/guest/guest.py stop >/dev/null 2>&1' EXIT
: > "$out/qmp.txt"

# A. With the adapter.
python3 plan/tools/guest/guest.py start "$image" || exit 1
if python3 plan/tools/guest/guest.py wait --timeout 240; then
	guest "service status networking; service status sshd; ifconfig ue0" > "$out/a.txt"
	cat "$out/a.txt"
	grep -qi 'networking.*completed' "$out/a.txt" && pass a-networking-completed || fail a-networking-completed
	grep -q 'inet 10\.0\.2\.' "$out/a.txt" && pass a-address || fail a-address
else
	fail a-ssh
fi
python3 plan/tools/guest/guest.py stop >/dev/null 2>&1

# B. Without it at boot: taken out as soon as the monitor answers.
python3 plan/tools/guest/guest.py start "$image" || exit 1
i=0
while [ $i -lt 50 ]; do
	if send device_del '{"id":"ecm"}'; then
		break
	fi
	sleep 0.1
	i=$((i + 1))
done
[ $i -lt 50 ] && pass b-adapter-out || fail b-adapter-out
sleep 75
send device_add '{"driver":"usb-net","bus":"xhci.0","port":"2","netdev":"net0","id":"ecm2","mac":"52:54:00:33:00:01","msos-desc":true}'
if python3 plan/tools/guest/guest.py wait --timeout 120; then
	pass b-boot-went-on
	guest "service status networking; service status sshd; ifconfig -a" > "$out/b.txt"
	cat "$out/b.txt"
	grep -i 'networking' "$out/b.txt" | grep -qi 'completed' && fail b-networking-gave-up || pass b-networking-gave-up
	grep -qi 'sshd.*running' "$out/b.txt" && pass b-sshd-running || fail b-sshd-running
else
	fail b-boot-went-on
fi

[ $status = 0 ] && echo "networking-wait: PASS" || echo "networking-wait: FAIL"
exit $status
