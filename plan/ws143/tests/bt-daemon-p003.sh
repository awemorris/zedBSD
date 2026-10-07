#!/bin/sh
# ws143-p003: bluetoothd and bt on a running zedBSD guest of plan/ws143/tests/config-amd64-bt.mk (the test kernel's
# loopback controller /dev/bt0; QEMU has no Bluetooth controller).
#  1. bluetoothd (started here, not by rc) is ready on the loopback controller: bt show says ready, its address
#     00:11:22:33:44:55, LE and the P-256 and DHKey commands.
#  2. bt scan 3 finds exactly the loopback's four devices with their fields: the extended result's
#     "Loopback Keyboard" (class 0x002540), the RSSI result (0A:0B:0C:0D:0E:02, class 0x002580, RSSI -60), the public
#     report's "Loopback Mouse" (appearance 0x03c2) and the random report (4A:0B:0C:0D:0E:04).
#  3. A user that is not root (kei, else nobody) is refused the scan (ERROR permission) but may show.
#  4. The node going under the daemon: with the daemon stopped, bt-probe -W 3000 asks the loopback controller to
#     withdraw itself in 3 s; the daemon started at once is ready, sees the node go (closed, lost) and is ready again
#     on the controller's return.
#  5. With the daemon stopped, bt-probe -L (p002's class test) still passes with the loopback's new commands.
# PASS: every "ok" line and the last line bt-daemon-p003: PASS.
#
#   (a guest up through plan/tools/guest/guest.py, e.g. plan/tools/files/files-guest.sh start IMAGE)
#   plan/ws143/tests/bt-daemon-p003.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 | tr -d '\r'; }
status=0
expect() {
	if [ "$2" = "$3" ]; then echo "ok: $1"; else echo "FAIL: $1 (got '$2', want '$3')"; status=1; fi
}
has() {
	if printf '%s\n' "$2" | grep -qF -- "$3"; then echo "ok: $1"; else echo "FAIL: $1 (no '$3')"; status=1; fi
}
stop_daemon() {
	guest 'for p in $(ps -A -o pid,args | grep "[/]sbin/bluetoothd" | awk "{print \$1}"); do kill $p; done; sleep 1; true' >/dev/null
}

# 1. The daemon, ready.
stop_daemon
guest '/sbin/bluetoothd >/tmp/btd.log 2>&1 & sleep 3; true' >/dev/null
show=$(guest '/bin/bt show')
printf '%s\n' "$show"
has "bt show is ready" "$show" "BT SHOW state=ready"
has "the loopback's address" "$show" "address=00:11:22:33:44:55"
has "LE, P-256 and DHKey" "$show" "le=1 p256=1 dhkey=1"
has "the daemon logged its start" "$(guest 'cat /tmp/btd.log')" "BLUETOOTHD READY state=ready"

# 2. The scan.
scan=$(guest '/bin/bt scan 3')
printf '%s\n' "$scan"
has "four devices" "$scan" "BT SCAN devices=4"
has "the keyboard" "$scan" 'address=0A:0B:0C:0D:0E:01 type=bredr rssi=-40 class=0x002540 name="Loopback Keyboard"'
has "the RSSI result" "$scan" 'address=0A:0B:0C:0D:0E:02 type=bredr rssi=-60 class=0x002580 name=""'
has "the mouse" "$scan" 'address=0A:0B:0C:0D:0E:03 type=le-public rssi=-50 appearance=0x03c2 name="Loopback Mouse"'
has "the random report" "$scan" 'address=4A:0B:0C:0D:0E:04 type=le-random rssi=-70 name=""'

# 3. Not for another user.
user=$(guest 'grep -q "^kei:" /etc/passwd && echo kei || echo nobody' | tail -1)
has "$user is refused the scan" "$(guest "runas $user /bin/bt scan 1; true")" "ERROR permission"
has "$user may show" "$(guest "runas $user /bin/bt show; true")" "BT SHOW state=ready"

# 4. The node going under the daemon, and coming back.
stop_daemon
has "the withdrawal is due in 3 s" "$(guest '/bin/bt-probe -W 3000')" "BT WITHDRAW delay_ms=3000"
guest '/sbin/bluetoothd >/tmp/btd2.log 2>&1 & sleep 1; /bin/bt show' | grep -q 'BT SHOW state=ready' && echo "ok: ready before the withdrawal" || { echo "FAIL: not ready before the withdrawal"; status=1; }
guest 'sleep 6; true' >/dev/null
log=$(guest 'cat /tmp/btd2.log')
printf '%s\n' "$log"
has "the daemon saw the node go" "$log" "closed (lost"
has "and is ready again" "$(guest '/bin/bt show')" "BT SHOW state=ready"
expect "it started twice on the controller" "$(printf '%s\n' "$log" | grep -c 'state=ready ')" 2

# 5. p002's class test, the daemon stopped.
stop_daemon
guest '/bin/bt-probe -L' | grep -q '^BT LOOPBACK PASS$' && echo "ok: bt-probe -L" || { echo "FAIL: bt-probe -L"; status=1; }

[ $status = 0 ] && echo "bt-daemon-p003: PASS" || echo "bt-daemon-p003: FAIL"
exit $status
