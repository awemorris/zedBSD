#!/bin/sh
# A crashed session starting again, and the PIN on the login and lock screens, on the Venus guest of the graphical
# login image (plan/ws035/tests/build-login-image.sh BUILD graphical: user is logged in at boot, PIN 0000).
# The guest's own files are read over SSH (sessiond's log, the session's and the greeter's), never the console:
#  1. The session compositor killed while unlocked: sessiond starts user's session again without the login screen,
#     unlocked (desktop-restart.png).
#  2. Locked (Super+L) and killed: the session starts again with the lock screen (lock-restart.png); a swipe up
#     brings the card without unlocking, a key goes into the PIN's field with its keypad (lock-pin.png), and the
#     keypad's 0 three times and OK unlock with 0000.
#  3. Killed again until sessiond refuses (the third crash in two minutes): the login screen, with the PIN and the
#     keypad by default (greeter-pin.png).
#  4. A wrong PIN typed (its try kept in /var/db/sessiond/pin-tries), then 0000 on the keypad logs user in
#     (pin-login.png); the tries are cleared.
#
#   GUEST_RUNTIME=DIR VENUS_RENDERER=DIR plan/ws035/tests/zdesktop-guest.sh start BUILD/hdd-image.img
#   GUEST_RUNTIME=DIR sh plan/tools/sessiond/restart-guest.sh OUTDIR
# The last line is "restart-guest: PASS" or "restart-guest: FAIL N".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
: "${GUEST_RUNTIME:?the guest's runtime directory}"
export GUEST_RUNTIME
out=${1:?OUTDIR}
mkdir -p "$out"
failures=0
guest() { timeout 60 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
shot() { timeout 60 python3 plan/ws035/tests/zdesktop-check.py "$out/$1" --runtime "$GUEST_RUNTIME" >/dev/null 2>&1 || echo "shot $1 FAILED"; }
keys() { timeout 30 python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; sleep 0.8; }
pointer() { timeout 30 python3 plan/ws035/tests/qmp-pointer.py --width 1280 --height 800 "$GUEST_RUNTIME/qmp.sock" "$@"; }
pass() { echo "ok $1"; }
fail() { echo "FAIL $1"; failures=$((failures + 1)); }

# How many lines of a guest file match a pattern.
count() { guest "grep -cE '$2' $1 2>/dev/null || true" | tail -1; }

# Waits (seconds) until a guest file has at least N lines matching a pattern.
wait_count() {
	tries=0
	while [ "$tries" -lt "$4" ]; do
		found=$(count "$1" "$2")
		[ "${found:-0}" -ge "$3" ] 2>/dev/null && { pass "$5"; return 0; }
		tries=$((tries + 1))
		sleep 1
	done
	fail "$5 (wanted $3 of '$2' in $1, have ${found:-0})"
	return 1
}

# The session compositor's process: user's /bin/wayland.
session_pid() { guest "ps -A -o pid,uid,comm" | awk '$2 == 1000 && $3 == "/bin/wayland" { print $1 }' | head -1; }

# Kills the session compositor as a crash would.
crash() {
	pid=$(session_pid)
	if [ -z "$pid" ]; then
		fail "no session compositor to kill"
		return 1
	fi
	guest "kill -9 $pid" >/dev/null
	echo "killed $pid"
}

# Presses the keypad's key at ROW COLUMN (0-based; the digits 1-9, then OK 0 Backspace) of the last keypad a log says.
keypad() {
	set -- "$1" "$2" $(guest "grep 'KWL GREETER keypad keys=' $3 | tail -1" | sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\) right=\([0-9]*\) bottom=\([0-9]*\).*/\1 \2 \3 \4/p')
	if [ $# -lt 6 ]; then
		fail "no keypad in $3"
		return 1
	fi
	width=$(( ($5 - $3 - 12) / 3 ))
	x=$(( $3 + $2 * (width + 6) + width / 2 ))
	y=$(( $4 + $1 * 46 + 20 ))
	pointer move "$x" "$y" sleep 150 down sleep 60 up sleep 400
}

# 0. The boot's automatic login: user's desktop.
wait_count /var/log/sessiond.log 'SESSIOND HANDOFF go' 1 90 "boot session took the display"
sleep 3
shot desktop-boot.png

# 1. A crash while unlocked: the same user's session again, without the login screen and unlocked.
crash
wait_count /var/log/sessiond.log 'SESSIOND SESSION end user=user .*crashed=1 locked=0' 1 20 "crash 1 seen, unlocked"
wait_count /var/log/sessiond.log 'SESSIOND SESSION restart user=user locked=0 crashes=1' 1 20 "crash 1 restarts unlocked"
wait_count /var/log/sessiond.log 'SESSIOND HANDOFF go' 2 60 "restarted session took the display"
locked=$(count /run/user/1000/session.log 'KWL LOCK locked')
[ "${locked:-1}" -eq 0 ] && pass "restarted session not locked" || fail "restarted session locked ($locked)"
sleep 3
shot desktop-restart.png

# 2. Locked, then a crash: the session starts again with the lock screen.
keys '<super-l>'
wait_count /run/user/1000/session.log 'KWL LOCK locked reason=key' 1 10 "Super+L locks"
wait_count /var/log/sessiond.log 'SESSIOND SESSION locked user=user' 1 10 "sessiond told LOCKED"
crash
wait_count /var/log/sessiond.log 'SESSIOND SESSION restart user=user locked=1' 1 20 "crash 2 restarts locked"
wait_count /var/log/sessiond.log 'SESSIOND HANDOFF go' 3 60 "locked session took the display"
wait_count /run/user/1000/session.log 'KWL LOCK locked reason=restart' 1 20 "restarted with the lock screen"
sleep 3
shot lock-restart.png

# A swipe up on a lock the session did not choose (the crash's) brings the card, never opens the lock.
pointer move 640 780 sleep 100 down sleep 60 move 640 600 sleep 60 move 640 300 sleep 60 up sleep 800
wait_count /run/user/1000/session.log 'KWL LOCK swipe via=.* grace=0' 1 10 "a swipe has no grace after the crash"
unlocks=$(count /run/user/1000/session.log 'KWL LOCK unlocked')
[ "${unlocks:-1}" -eq 0 ] && pass "the swipe did not unlock" || fail "the swipe unlocked"

# The card: a key goes into the PIN's field, then the keypad's 0, 0, 0 and OK.
keys '0'
wait_count /run/user/1000/session.log 'KWL GREETER styles=3' 1 10 "lock screen offers the PIN"
wait_count /run/user/1000/session.log 'KWL GREETER keypad keys=12' 1 10 "lock screen shows the PIN's keypad"
sleep 1
shot lock-pin.png
keypad 3 1 /run/user/1000/session.log
keypad 3 1 /run/user/1000/session.log
keypad 3 1 /run/user/1000/session.log
keypad 3 0 /run/user/1000/session.log
wait_count /var/log/sessiond.log 'SESSIOND UNLOCK ok user=user style=pin' 1 10 "PIN 0000 unlocks"
wait_count /var/log/sessiond.log 'SESSIOND SESSION unlocked user=user' 1 10 "sessiond told UNLOCKED"
sleep 2
shot unlocked.png

# 3. Crashes until sessiond refuses: the login screen.
kills=0
while [ "$kills" -lt 3 ]; do
	crash
	kills=$((kills + 1))
	sleep 4
	refused=$(count /var/log/sessiond.log 'SESSIOND SESSION restart refused user=user')
	[ "${refused:-0}" -ge 1 ] && break
	wait_count /var/log/sessiond.log 'SESSIOND HANDOFF go' $((3 + kills)) 60 "restart after kill $kills" >/dev/null
done
echo "kills after the lock test: $kills"
wait_count /var/log/sessiond.log 'SESSIOND SESSION restart refused user=user crashes=3' 1 10 "the third crash in the window refused"
wait_count /var/log/greeter.log 'KWL GREETER open' 1 60 "the login screen came"
wait_count /var/log/greeter.log 'KWL GREETER styles=3' 1 20 "login screen offers the PIN"
wait_count /var/log/greeter.log 'KWL GREETER keypad keys=12' 1 20 "login screen shows the PIN's keypad"
sleep 3
shot greeter-pin.png

# 4. A wrong PIN typed, kept in the file; then 0000 on the keypad logs in and clears it.
keys '1111' '\n'
wait_count /var/log/sessiond.log 'SESSIOND AUTH fail user=user wrong=1 delay=2 style=pin' 1 10 "a wrong PIN refused"
tries=$(guest "cat /var/db/sessiond/pin-tries; stat -f '%Sp %u' /var/db/sessiond/pin-tries 2>/dev/null || ls -l /var/db/sessiond/pin-tries")
echo "pin-tries after the wrong PIN: $tries" | tr '\n' ' '
echo
case "$tries" in *"1000 1"*) pass "the wrong try kept in the file" ;; *) fail "the wrong try not in the file" ;; esac
sleep 3
keypad 3 1 /var/log/greeter.log
keypad 3 1 /var/log/greeter.log
keypad 3 1 /var/log/greeter.log
keypad 3 1 /var/log/greeter.log
keypad 3 0 /var/log/greeter.log
wait_count /var/log/sessiond.log 'SESSIOND AUTH ok user=user uid=1000 style=pin' 1 10 "PIN 0000 logs in"
wait_count /var/log/sessiond.log 'SESSIOND HANDOFF go' $((3 + kills)) 60 "the login's session took the display"
tries=$(guest "cat /var/db/sessiond/pin-tries")
case "$tries" in *"1000 "*) fail "the tries not cleared ($tries)" ;; *) pass "the tries cleared" ;; esac
sleep 3
shot pin-login.png

# The logs, for the record.
guest "cat /var/log/sessiond.log" > "$out/sessiond.log"
guest "cat /var/log/greeter.log" > "$out/greeter.log"
guest "cat /run/user/1000/session.log" > "$out/session.log"
if [ "$failures" -eq 0 ]; then
	echo "restart-guest: PASS"
	exit 0
fi
echo "restart-guest: FAIL $failures"
exit 1
