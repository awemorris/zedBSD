#!/bin/sh
# ws143-p002: the Bluetooth HCI class on a running zedBSD guest of plan/ws143/tests/config-amd64-bt.mk (the test kernel's
# loopback controller; QEMU has no Bluetooth controller).
#  1. /dev/bt0 is there, mode 0600 root, and the kernel's log names the loopback controller; with no USB controller, no
#     other node (/dev/bt1) and no usb-bt attach.
#  2. bt-probe -L: one open (EBUSY), a read of nothing, the two queues' order, EMSGSIZE, an ACL packet sent and back,
#     SCO refused, a flood larger than the queue (stalls counted, nothing dropped, the order kept), the reset's notice
#     in a full queue at its place, the controller withdrawn under a waiting read (ENODEV, POLLHUP, GET_INFO still
#     given) and published again as bt0 ("BT LOOPBACK PASS").
#  3. bt-probe (the plain probe) answers on the loopback controller ("BT PASS").
#  4. A user that is not root (kei, else nobody) cannot open it.
# PASS: every "ok" line and the last line bt-loopback-p002: PASS.
#
#   (a guest up through plan/tools/guest/guest.py, e.g. plan/tools/files/files-guest.sh start IMAGE)
#   plan/ws143/tests/bt-loopback-p002.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 | tr -d '\r'; }
status=0
expect() {
	if [ "$2" = "$3" ]; then echo "ok: $1"; else echo "FAIL: $1 (got '$2', want '$3')"; status=1; fi
}

# 1. The node, and no other.
expect "/dev/bt0 is a character device" "$(guest 'test -c /dev/bt0 && echo yes' | tail -1)" yes
expect "it is root's alone" "$(guest 'ls -l /dev/bt0' | tail -1 | cut -c1-10)" "crw-------"
expect "the kernel published the loopback controller" "$(guest 'dmesg | grep -c "bt-hci: /dev/bt0: Loopback Bluetooth controller"' | tail -1)" 1
expect "no other controller's node" "$(guest 'test -e /dev/bt1 && echo yes || echo no' | tail -1)" no
expect "no USB controller attached" "$(guest 'dmesg | grep -c "usb-bt: "' | tail -1)" 0

# 2. The class's test.
guest '/bin/bt-probe -L' | tee /dev/stderr | grep -q '^BT LOOPBACK PASS$' && echo "ok: bt-probe -L" || { echo "FAIL: bt-probe -L"; status=1; }
expect "the withdrawal and the return were logged" "$(guest 'dmesg | grep -c "bt-loopback: test controller published again"' | tail -1)" 1

# 3. The plain probe.
guest '/bin/bt-probe -f /dev/bt0' | tee /dev/stderr | grep -q '^BT PASS$' && echo "ok: bt-probe" || { echo "FAIL: bt-probe"; status=1; }

# 4. Not for another user.
user=$(guest 'grep -q "^kei:" /etc/passwd && echo kei || echo nobody' | tail -1)
guest "runas $user /bin/bt-probe -f /dev/bt0 2>&1; true" | grep -q 'FAIL step=open' && echo "ok: $user cannot open it" || { echo "FAIL: $user could open it (or runas is missing)"; status=1; }

[ $status = 0 ] && echo "bt-loopback-p002: PASS" || echo "bt-loopback-p002: FAIL"
exit $status
