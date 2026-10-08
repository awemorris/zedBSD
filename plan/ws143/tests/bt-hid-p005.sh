#!/bin/sh
# ws143-p005 i02: bluetoothd's HID host over BR/EDR on a running zedBSD guest of plan/ws143/tests/build-bt-image.sh (the
# test kernel's loopback controller /dev/bluetooth0 plays the HID devices of plan/ws143/phase005/phase.md section 6:
# 0A:0B:0C:0D:0E:01 a boot keyboard, 02 a boot mouse that reconnects by itself).  As root over SSH; the console and serial
# logs are not read.  The daemon runs with -s (its btsnoop record).
#  1. bt scan, then bt pair 01 (CONFIRM y): PAIRED, and the pairing's link handed to the HID host (no disconnection):
#     bt status says open with its input node; evdev-probe (by its place .../0A:0B:0C:0D:0E:01) reads KEY_A 1 and 0, KEY_B 1.
#     bt disconnect 01: KEY_B 0 and a frame, then the node goes; bt status says reconnect=off.  bt connect 01: connected.
#  2. bt pair 02 (CONSENT y): handed over, open, REL_X 5 every second; 4 s in, the mouse goes (supervision timeout) and a
#     second later connects by itself, asking for its control channel before any security (Pending, phase005 §9.8):
#     a new node with REL_X 5 again.  bt disconnect 02, then bt connect 02 is unreachable (the mouse is not connectable).
#  3. Permissions: btuser may not connect but may read the status.
#  4. PAIR while a HID connection is under way is busy (a race: noted, not failed, when the connection was too quick).
#  4b. (i03) The LE HOG mouse 04, bonded by the test (its bond's file written as root: LTK 00 11 .. FF): bt connect 04
#     le-public connects it over HOGP (transport=hog, battery=80), evdev-probe reads REL_X 5 every second.
#  5. The daemon started again: 01 (bluetoothd pages it) is open by itself, 02 (it comes by itself) waits, 04 is open
#     by the auto-connect (its filter accept list).
#  6. The controller gone (bt-probe -W, the loopback withdraws itself): 01's node goes with KEY_B released, the daemon
#     sees the node go, and 01 and 04 are open again on the controller's return.
#  7. bt forget 01 while open: its node goes, its bond and HID record are gone.  bt forget 02 and 04 (cleanup).
#  8. The btsnoop record starts with its header and holds the traffic (copied out to the run's folder for tshark).
#  9. The guest's USB input nodes are still there (the Bluetooth nodes did not disturb them).
# PASS: every "ok" line and the last line bt-hid-p005: PASS.  Run bt-loopback-p002.sh, bt-daemon-p003.sh and
# bt-pair-p004.sh too (unchanged: the router, the loopback's per-device connections and the handoff must not change them).
#
#   plan/ws143/tests/build-bt-image.sh BUILD; plan/tools/files/files-guest.sh start BUILD/hdd-image.img
#   plan/ws143/tests/bt-hid-p005.sh [OUTDIR]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
export GUEST_RUNTIME="${GUEST_RUNTIME:-$PWD/build/ws071-run}"
. plan/tools/fresh-out.sh
fresh_out "${1:-build/ws143-bt-hid-p005}"
out=$fresh_dir
status=0
guest() { timeout 90 python3 plan/tools/guest/guest.py run "$1" 2>&1 | tr -d '\r'; }
ok() { echo "ok: $1"; }
fail() { echo "FAIL: $1${2:+ ($2)}"; status=1; }
has() {
	if printf '%s\n' "$2" | grep -qF -- "$3"; then ok "$1"; else fail "$1" "no '$3' in: $(printf '%s' "$2" | tr '\n' '|')"; fi
}
expect() { if grep -q -- "$2" "$3"; then ok "$1"; else fail "$1" "no '$2' in $3"; fi; }
stop_daemon() {
	guest 'service stop bluetoothd >/dev/null 2>&1; for p in $(ps -A -o pid,args | grep "[/]sbin/bluetoothd" | awk "{print \$1}"); do kill $p; done; sleep 2; true' >/dev/null
}
start_daemon() {
	guest "/sbin/bluetoothd -s $1 >$2 2>&1 & sleep 3; true" >/dev/null
}
folder=/var/db/bluetooth/00:11:22:33:44:55
keyboard=0A:0B:0C:0D:0E:01
mouse=0A:0B:0C:0D:0E:02
hog=0A:0B:0C:0D:0E:04

# 0. A daemon of its own, without the HID devices' bonds of an earlier run, and the loopback's devices as at power-on
#    (a withdrawal forgets that the mouse was paired).
stop_daemon
guest "rm -f $folder/$keyboard-bredr $folder/$keyboard-bredr.hid $folder/$mouse-bredr $folder/$mouse-bredr.hid $folder/$hog-le-public $folder/$hog-le-public.hid /tmp/btd-p005*.snoop; true" >/dev/null
has "the loopback withdraws and comes back" "$(guest '/bin/bt-probe -W 100; sleep 2')" "BT WITHDRAW delay_ms=100"
start_daemon /tmp/btd-p005.snoop /tmp/btd-p005.log
has "bt show is ready" "$(guest '/bin/bt show')" "BT SHOW state=ready"

# 1. The keyboard: paired, handed over, its keys; disconnected; connected again.
guest '/bin/bt scan 3' > "$out/scan.txt"
expect "the scan saw the keyboard's class" "address=$keyboard type=bredr rssi=-40 class=0x002540" "$out/scan.txt"
guest "/bin/evdev-probe -b bluetooth -p $keyboard -N -t 30000 -r 20000 > /tmp/p005-kbd1.txt 2>&1 </dev/null & echo started" >/dev/null
guest "echo y | /bin/bt pair $keyboard" > "$out/pair-keyboard.txt"
cat "$out/pair-keyboard.txt"
expect "the keyboard paired" "PAIRED address=$keyboard type=bredr authenticated=1" "$out/pair-keyboard.txt"
sleep 2
guest '/bin/bt status' > "$out/status1.txt"
cat "$out/status1.txt"
expect "the keyboard handed over and open" "HID address=$keyboard type=bredr transport=hid state=open input=/dev/input/event" "$out/status1.txt"
sleep 1
guest "/bin/bt disconnect $keyboard" > "$out/disconnect-keyboard.txt"
expect "bt disconnect" "BT DISCONNECT result=ok" "$out/disconnect-keyboard.txt"
sleep 2
guest 'cat /tmp/p005-kbd1.txt' > "$out/keyboard1.txt"
expect "the keyboard's node, Bluetooth's bus" "^EVDEV node=.*bus=5 vendor=1209 product=4b42" "$out/keyboard1.txt"
expect "KEY_A down" '^EVDEV event type=1 code=30 value=1$' "$out/keyboard1.txt"
expect "KEY_A up" '^EVDEV event type=1 code=30 value=0$' "$out/keyboard1.txt"
expect "KEY_B down" '^EVDEV event type=1 code=48 value=1$' "$out/keyboard1.txt"
tail_lines=$(sed -n '/code=48 value=1$/,$p' "$out/keyboard1.txt" | tr '\n' '|')
case "$tail_lines" in
*"type=1 code=48 value=0|EVDEV event type=0 code=0 value=0|"*"EVDEV gone|"*) ok "KEY_B released before the node went" ;;
*) fail "KEY_B released before the node went" "$tail_lines" ;;
esac
guest '/bin/bt status' > "$out/status2.txt"
expect "not wanted back after DISCONNECT" "address=$keyboard .*state=idle .*reconnect=off" "$out/status2.txt"
guest "/bin/bt connect $keyboard" > "$out/connect-keyboard.txt"
cat "$out/connect-keyboard.txt"
expect "bt connect" "BT CONNECT result=connected input=/dev/input/event" "$out/connect-keyboard.txt"

# 2. The mouse: paired and handed over, it goes and comes back by itself; then not connectable.
guest "/bin/evdev-probe -b bluetooth -p $mouse -N -t 30000 -r 10000 > /tmp/p005-mouse1.txt 2>&1 </dev/null & echo started" >/dev/null
guest "echo y | /bin/bt pair $mouse" > "$out/pair-mouse.txt"
cat "$out/pair-mouse.txt"
expect "the mouse asks for agreement" "CONSENT" "$out/pair-mouse.txt"
expect "the mouse paired" "PAIRED address=$mouse type=bredr authenticated=0" "$out/pair-mouse.txt"
# The mouse goes 4 s into its connection and comes back a second later; the node it comes back with reuses the
# number of the first (the lowest free), so its reader starts after it is back (-N would take the name as not new).
sleep 8
guest "/bin/evdev-probe -b bluetooth -p $mouse -t 5000 -r 4000 > /tmp/p005-mouse2.txt 2>&1 </dev/null & echo started" >/dev/null
sleep 3
guest 'cat /tmp/p005-mouse1.txt' > "$out/mouse1.txt"
guest 'cat /tmp/p005-mouse2.txt' > "$out/mouse2.txt"
guest '/bin/bt status' > "$out/status3.txt"
cat "$out/status3.txt"
expect "the mouse's node" "^EVDEV node=.*bus=5 vendor=1209 product=4d53" "$out/mouse1.txt"
expect "REL_X 5" '^EVDEV event type=2 code=0 value=5$' "$out/mouse1.txt"
expect "the mouse went by itself" '^EVDEV gone$' "$out/mouse1.txt"
expect "the mouse came back by itself: its node again" "^EVDEV node=.*bus=5 vendor=1209 product=4d53" "$out/mouse2.txt"
expect "REL_X 5 again" '^EVDEV event type=2 code=0 value=5$' "$out/mouse2.txt"
expect "the mouse is open again" "address=$mouse type=bredr transport=hid state=open" "$out/status3.txt"
since=$(sed -n "s/.*address=$mouse .*state=open .*since=\([0-9]*\) .*/\1/p" "$out/status3.txt")
if [ -n "$since" ] && [ "$since" -le 8 ]; then ok "open again since its own connection (${since} s)"; else fail "open again since its own connection" "since=${since:-?}"; fi
has "bt disconnect of the mouse" "$(guest "/bin/bt disconnect $mouse")" "BT DISCONNECT result=ok"
has "the mouse cannot be paged" "$(guest "/bin/bt connect $mouse; true")" "ERROR unreachable"

# 3. Permissions.
has "btuser may not connect" "$(guest "runas btuser /bin/bt connect $keyboard 2>&1; true")" "ERROR permission"
has "btuser may read the status" "$(guest 'runas btuser /bin/bt status 2>&1; true')" "BT STATUS devices=2"

# 4. A pairing while a connection is under way.
has "bt disconnect of the keyboard" "$(guest "/bin/bt disconnect $keyboard")" "BT DISCONNECT result=ok"
sleep 1
guest "/bin/bt connect $keyboard > /tmp/p005-connect.txt 2>&1 & echo y | /bin/bt pair 0A:0B:0C:0D:0E:07; sleep 3; cat /tmp/p005-connect.txt; true" > "$out/busy.txt"
cat "$out/busy.txt"
if grep -q "ERROR busy" "$out/busy.txt"; then
	ok "PAIR during a connection is busy"
elif grep -q "BT PAIR result=paired" "$out/busy.txt"; then
	echo "note: the connection ended before the pairing asked (no race this time; the host test checks busy)"
else
	fail "PAIR during a connection is busy" "neither busy nor paired"
fi
# Whichever went first, the keyboard connects (at once when it is open already).
has "the keyboard connected" "$(guest "sleep 2; /bin/bt connect $keyboard")" "BT CONNECT result=connected"

# 4b. The LE HOG mouse: its bond written as root (the daemon's account owns it), then connected over HOGP.
guest "printf 'type=le-public\nname=HOG Mouse\nltk=00112233445566778899aabbccddeeff\nediv=0\nrand=0000000000000000\nkey_size=16\nauthenticated=0\nsecure=1\nlegacy=0\n' > $folder/$hog-le-public && chown 80 $folder/$hog-le-public && chmod 600 $folder/$hog-le-public; ls -ln $folder/$hog-le-public" > "$out/hog-bond.txt"
expect "the HOG mouse's bond, _bluetooth's" '^-rw------- *[0-9]* *80 ' "$out/hog-bond.txt"
guest "/bin/evdev-probe -b bluetooth -p $hog -N -t 20000 -r 5000 > /tmp/p005-hog1.txt 2>&1 </dev/null & echo started" >/dev/null
guest "/bin/bt connect $hog le-public" > "$out/connect-hog.txt"
cat "$out/connect-hog.txt"
expect "the HOG mouse connected over HOGP" "CONNECTED address=$hog type=le-public transport=hog input=/dev/input/event" "$out/connect-hog.txt"
expect "bt connect of the HOG mouse" "BT CONNECT result=connected input=/dev/input/event" "$out/connect-hog.txt"
sleep 4
guest '/bin/bt status' > "$out/status-hog.txt"
guest 'cat /tmp/p005-hog1.txt' > "$out/hog1.txt"
expect "the HOG mouse open, its battery read" "address=$hog type=le-public transport=hog state=open .*battery=80" "$out/status-hog.txt"
expect "the HOG mouse's node, Bluetooth's bus" "^EVDEV node=.*bus=5 vendor=1209 product=4842" "$out/hog1.txt"
expect "REL_X 5 from its notifications" '^EVDEV event type=2 code=0 value=5$' "$out/hog1.txt"

# 5. The daemon again: the keyboard comes back (paged), the mouse waits for itself, the HOG mouse comes by the auto-connect.
stop_daemon
start_daemon /tmp/btd-p005b.snoop /tmp/btd-p005b.log
sleep 3
guest '/bin/bt status' > "$out/status4.txt"
cat "$out/status4.txt"
expect "the keyboard open after the restart" "address=$keyboard .*state=open" "$out/status4.txt"
expect "the mouse waits" "address=$mouse .*state=waiting" "$out/status4.txt"
expect "the HOG mouse open by the auto-connect" "address=$hog .*state=open" "$out/status4.txt"

# 6. The controller gone and back.
stop_daemon
has "the withdrawal is due in 10 s" "$(guest '/bin/bt-probe -W 10000')" "BT WITHDRAW delay_ms=10000"
start_daemon /tmp/btd-p005c.snoop /tmp/btd-p005c.log
sleep 1
guest "/bin/evdev-probe -b bluetooth -p $keyboard -t 10000 -r 20000 > /tmp/p005-kbd2.txt 2>&1 </dev/null & echo started" >/dev/null
sleep 11
guest 'cat /tmp/p005-kbd2.txt' > "$out/keyboard2.txt"
guest 'cat /tmp/btd-p005c.log' > "$out/daemon-lost.log"
expect "the keyboard's node before the withdrawal" "^EVDEV node=.*bus=5 vendor=1209 product=4b42" "$out/keyboard2.txt"
# The reader came after "b" was pressed (no press line): the release, a frame, then the end.
tail_lines=$(sed -n '/code=48 value=0$/,$p' "$out/keyboard2.txt" | tr '\n' '|')
case "$tail_lines" in
*"type=1 code=48 value=0|EVDEV event type=0 code=0 value=0|"*"EVDEV gone|"*) ok "KEY_B released as the controller went" ;;
*) fail "KEY_B released as the controller went" "$tail_lines" ;;
esac
expect "the daemon saw the node go" "closed (lost" "$out/daemon-lost.log"
sleep 3
guest '/bin/bt status' > "$out/status5.txt"
expect "the keyboard open on the controller's return" "address=$keyboard .*state=open" "$out/status5.txt"
expect "the HOG mouse open on the controller's return" "address=$hog .*state=open" "$out/status5.txt"

# 7. FORGET while open, then the mouse's (cleanup).
guest "/bin/evdev-probe -b bluetooth -p $keyboard -t 5000 -r 10000 > /tmp/p005-kbd3.txt 2>&1 </dev/null & echo started" >/dev/null
sleep 1
has "bt forget of the open keyboard" "$(guest "/bin/bt forget $keyboard")" "BT FORGET result=ok"
sleep 2
guest 'cat /tmp/p005-kbd3.txt' > "$out/keyboard3.txt"
expect "its node went" '^EVDEV gone$' "$out/keyboard3.txt"
count=$(guest "ls $folder | grep -c $keyboard; true" | tail -1)
[ "$count" = 0 ] && ok "its bond and record are gone" || fail "its bond and record are gone" "$count files"
has "bt forget of the mouse" "$(guest "/bin/bt forget $mouse")" "BT FORGET result=ok"
has "bt forget of the HOG mouse" "$(guest "/bin/bt forget $hog le-public")" "BT FORGET result=ok"
count=$(guest "ls $folder | grep -c 'hid\$'; true" | tail -1)
[ "$count" = 0 ] && ok "no HID record left" || fail "no HID record left" "$count records"

# 8. The btsnoop record.
guest 'head -c 7 /tmp/btd-p005.snoop; echo; wc -c < /tmp/btd-p005.snoop' > "$out/snoop.txt"
expect "the record's header" "^btsnoop$" "$out/snoop.txt"
size=$(tail -1 "$out/snoop.txt" | tr -d ' ')
if [ "${size:-0}" -gt 4096 ] 2>/dev/null; then ok "the record holds the traffic ($size bytes)"; else fail "the record holds the traffic" "${size:-?} bytes"; fi
timeout 60 python3 plan/tools/guest/guest.py get /tmp/btd-p005.snoop "$out/btd-p005.snoop" >/dev/null 2>&1 &&
	echo "the btsnoop record: $out/btd-p005.snoop (tshark -r reads it)" || echo "note: the btsnoop record was not copied out"

# 9. The guest's USB input nodes.
guest '/bin/evdev-probe -l -b usb' > "$out/usb.txt"
usb=$(sed -n 's/^EVDEV count=//p' "$out/usb.txt")
if [ "${usb:-0}" -ge 1 ] 2>/dev/null; then ok "the USB input nodes are there ($usb)"; else fail "the USB input nodes are there" "${usb:-?}"; fi

stop_daemon
echo "outputs in $out"
[ $status = 0 ] && echo "bt-hid-p005: PASS" || echo "bt-hid-p005: FAIL"
exit $status
