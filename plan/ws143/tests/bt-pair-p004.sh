#!/bin/sh
# ws143-p004: bluetoothd's pairing on a running zedBSD guest of plan/ws143/tests/build-bt-image.sh (the test kernel's
# loopback controller /dev/bluetooth0 plays the devices of plan/ws143/phase004/phase.md section 8; QEMU has no Bluetooth).
#  1. The privilege separation: a root parent and a child as _bluetooth; bt show is ready with ssp=1 sc=1.
#  2. BR/EDR Numeric Comparison (0A:0B:0C:0D:0E:01): CONFIRM 123456 answered y, PAIRED authenticated=1 secure=1 and
#     the L2CAP probe; the bond's file is _bluetooth's, 0600, in a 0700 folder.  Again: the stored key (stored=1).
#  3. Just Works (07): CONSENT answered y, authenticated=0.  The debug key (05) and a key of 7 bytes (06) are refused
#     and not stored.  An unknown device (09) is unreachable.  FORGET, then a no to CONFIRM is rejected.  BONDS.
#  4. Permissions (D8): the test account btuser may not pair, scan or forget but may list the bonds; kei (wheel) may scan.
#  5. LE: the loopback's mouse (03) refuses the pairing (rejected); an LE device that never connects is cancelled
#     (timeout, 10 s).
#  6. The separation's life: the child killed ends the parent; the parent told to end (TERM) ends the child.
# PASS: every "ok" line and the last line bt-pair-p004: PASS.  Run bt-loopback-p002.sh and bt-daemon-p003.sh too.
#
#   (a guest up through plan/tools/guest/guest.py, e.g. plan/tools/files/files-guest.sh start IMAGE)
#   plan/ws143/tests/bt-pair-p004.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 | tr -d '\r'; }
status=0
expect() {
	if [ "$2" = "$3" ]; then echo "ok: $1"; else echo "FAIL: $1 (got '$2', want '$3')"; status=1; fi
}
has() {
	if printf '%s\n' "$2" | grep -qF -- "$3"; then echo "ok: $1"; else echo "FAIL: $1 (no '$3' in: $(printf '%s' "$2" | tr '\n' '|'))"; status=1; fi
}
lacks() {
	if printf '%s\n' "$2" | grep -qF -- "$3"; then echo "FAIL: $1 ('$3' in: $(printf '%s' "$2" | tr '\n' '|'))"; status=1; else echo "ok: $1"; fi
}
stop_daemon() {
	guest 'for p in $(ps -A -o pid,args | grep "[/]sbin/bluetoothd" | awk "{print \$1}"); do kill $p; done; sleep 2; true' >/dev/null
}
folder=/var/db/bluetooth/00:11:22:33:44:55

# 1. The separation, and the controller.
stop_daemon
expect "the test account is there" "$(guest 'grep -c "^btuser:" /etc/passwd' | tail -1)" 1
guest '/sbin/bluetoothd >/tmp/btd4.log 2>&1 & sleep 3; true' >/dev/null
processes=$(guest 'ps -A -o user,args | grep "[/]sbin/bluetoothd"')
printf '%s\n' "$processes"
has "a root parent" "$processes" "root"
has "a child as _bluetooth" "$processes" "_bluetooth"
show=$(guest '/bin/bt show')
has "bt show is ready" "$show" "BT SHOW state=ready"
has "SSP and SC on" "$show" "ssp=1 sc=1"
has "the child logged its uid" "$(guest 'cat /tmp/btd4.log')" "uid=80"

# 2. Numeric Comparison, the bond's file, the stored key.
pair=$(guest 'echo y | /bin/bt pair 0A:0B:0C:0D:0E:01')
printf '%s\n' "$pair"
has "the number 123456 asked" "$pair" "CONFIRM 123456"
has "paired, authenticated, the L2CAP probe answered" "$pair" "PAIRED address=0A:0B:0C:0D:0E:01 type=bredr authenticated=1 secure=1 legacy=0 key_size=16 stored=0 l2cap=1"
has "bt pair says paired" "$pair" "BT PAIR result=paired"
expect "the folder is _bluetooth's, 0700" "$(guest 'ls -ldn /var/db/bluetooth' | tail -1 | awk '{print $1, $3}')" "drwx------ 80"
expect "the bond's file is _bluetooth's, 0600" "$(guest "ls -ln $folder/0A:0B:0C:0D:0E:01-bredr" | tail -1 | awk '{print $1, $3}')" "-rw------- 80"
again=$(guest 'echo y | /bin/bt pair 0A:0B:0C:0D:0E:01')
has "the stored key, no question" "$again" "stored=1"
lacks "no number asked the second time" "$again" "CONFIRM"

# 3. Just Works, the refusals, FORGET, a no, BONDS.
works=$(guest 'echo y | /bin/bt pair 0A:0B:0C:0D:0E:07')
has "Just Works asks for agreement" "$works" "CONSENT"
has "Just Works is not authenticated" "$works" "authenticated=0 secure=1"
has "the debug key is refused" "$(guest 'echo y | /bin/bt pair 0A:0B:0C:0D:0E:05; true')" "ERROR debug-key"
has "a key of 7 bytes is refused" "$(guest 'echo y | /bin/bt pair 0A:0B:0C:0D:0E:06; true')" "ERROR key-size"
expect "neither is stored" "$(guest "ls $folder | grep -c -e 0A:0B:0C:0D:0E:05 -e 0A:0B:0C:0D:0E:06" | tail -1)" 0
has "an unknown device is unreachable" "$(guest 'echo y | /bin/bt pair 0A:0B:0C:0D:0E:09; true')" "ERROR unreachable"
has "FORGET" "$(guest '/bin/bt forget 0A:0B:0C:0D:0E:01')" "BT FORGET result=ok"
expect "its file is gone" "$(guest "test -e $folder/0A:0B:0C:0D:0E:01-bredr && echo there || echo gone" | tail -1)" gone
has "a no is rejected" "$(guest 'echo n | /bin/bt pair 0A:0B:0C:0D:0E:01; true')" "ERROR rejected"
bonds=$(guest '/bin/bt bonds')
has "BONDS lists Just Works' bond" "$bonds" 'BOND address=0A:0B:0C:0D:0E:07 type=bredr authenticated=0'
has "one bond" "$bonds" "BT BONDS bonds=1"
lacks "BONDS shows no key" "$bonds" "link_key"

# 4. Permissions.
has "btuser may not pair" "$(guest 'echo y | runas btuser /bin/bt pair 0A:0B:0C:0D:0E:07 2>&1; true')" "ERROR permission"
has "btuser may not scan" "$(guest 'runas btuser /bin/bt scan 1 2>&1; true')" "ERROR permission"
has "btuser may not forget" "$(guest 'runas btuser /bin/bt forget 0A:0B:0C:0D:0E:07 2>&1; true')" "ERROR permission"
has "btuser may list the bonds" "$(guest 'runas btuser /bin/bt bonds 2>&1; true')" "BT BONDS bonds=1"
has "kei (wheel) may scan (the loopback's five devices, 07 since T1-438)" "$(guest 'runas kei /bin/bt scan 2 2>&1; true')" "BT SCAN devices=5"

# 5. LE.
has "the LE mouse refuses the pairing" "$(guest 'echo y | /bin/bt pair 0A:0B:0C:0D:0E:03 le-public; true')" "ERROR rejected"
has "an LE device that never connects is cancelled" "$(guest 'echo y | /bin/bt pair 0A:0B:0C:0D:0E:09 le-public; true')" "ERROR timeout"
has "still ready" "$(guest '/bin/bt show')" "BT SHOW state=ready"

# 6. The separation's life.
guest 'kill $(ps -A -o user,pid,args | grep "^_bluetooth.*[/]sbin/bluetoothd" | awk "{print \$2}"); sleep 3; true' >/dev/null
left=$(guest 'ps -A -o user,pid,stat,args | grep "[/]sbin/bluetoothd"; true')
[ -n "$left" ] && printf 'left after the kill:\n%s\n' "$left"
expect "the child killed ends the parent" "$(guest 'ps -A -o args | grep -c "[/]sbin/bluetoothd"' | tail -1)" 0
guest '/sbin/bluetoothd >/tmp/btd5.log 2>&1 & sleep 3; true' >/dev/null
guest 'kill $(ps -A -o user,pid,args | grep "^root.*[/]sbin/bluetoothd" | awk "{print \$2}"); sleep 3; true' >/dev/null
expect "the parent told to end ends the child" "$(guest 'ps -A -o args | grep -c "[/]sbin/bluetoothd"' | tail -1)" 0

[ $status = 0 ] && echo "bt-pair-p004: PASS" || echo "bt-pair-p004: FAIL"
exit $status
