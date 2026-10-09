#!/bin/sh
# ws172-p002: the login and lock screens' PIN through sessiond and /sbin/passkey (docs/architecture/security.md,
# "Login authentication") on the Venus guest of plan/ws172/tests/build-passkey-image.sh, run like
# plan/ws035/tests/zdesktop-p102.sh: the boot's autologin session is stopped, the autologin emptied (restored at the
# end) and sessiond started, so the greeter comes up with kei (password "kei") selected.
#  0. /sbin/passkey is root's alone (-r-x------); /etc/passkey is made by the first enrollment, root's, 0600.
#  1. The greeter offers the password only (KWL GREETER styles=1); kei logs in with it (SESSIOND AUTH ok ...
#     style=password); the WS163 mock's ~/.config/keiland/pin, put there first, is removed by the session.
#  2. kei's PIN 246810 is enrolled through passkey (as sessiond's ENROLL pin would; Settings' path is host-tested).
#  3. Super+L: the lock screen offers the PIN (styles=3): locked.png ("PIN", "Use your password"); the PIN unlocks
#     (SESSIOND UNLOCK ok user=kei style=pin).
#  4. Five wrong PINs, each answered after sessiond's delay (wrong=1..5), turn the PIN off (styles=1): off.png.
#     The password unlocks (style=password); the next lock offers the PIN again and it unlocks.
#  5. The session ends: sessiond's new greeter offers kei the PIN (styles=3, counts kept in sessiond's memory) and
#     the PIN logs in (SESSIOND AUTH ok ... style=pin).
#  6. sessiond restarted: the greeter offers the password only again (styles=1) until kei logs in with it.
#  7. account-admin: a reset of pk2's password and pk2's removal each take pk2's PIN out of /etc/passkey.
#  8. No log has the PIN or a password.
#
#   plan/ws172/tests/build-passkey-image.sh BUILD; plan/tools/files/files-guest.sh start BUILD/hdd-image.img
#   plan/tools/guest/guest.py wait; sleep 30   (the boot's own sessiond settled: one sessiond only)
#   plan/ws172/tests/passkey-p002-guest.sh [OUTDIR]       (default build/ws172-passkey)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws071-run}"
export GUEST_RUNTIME
out=${1:-build/ws172-passkey}
mkdir -p "$out"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; sleep 0.8; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py --width 1280 --height 800 "$GUEST_RUNTIME/qmp.sock" "$@"; }
status=0
session=/run/user/1000/session.log
admin=/usr/libexec/account-admin

# Fails the run unless a guest file has a line matching a pattern (within some seconds).
expect_log() {
	tries=0
	found=0
	while [ $tries -lt "${3:-10}" ]; do
		found=$(guest "grep -cE '$2' $1" | tail -1)
		[ "${found:-0}" -gt 0 ] 2>/dev/null && break
		tries=$((tries + 1))
		sleep 1
	done
	if [ "${found:-0}" -gt 0 ] 2>/dev/null; then
		echo "log: $2 ok"
	else
		echo "log: $2 MISSING"
		status=1
	fi
}

# Fails the run unless a value matches a pattern.
expect() {
	if printf '%s\n' "$3" | grep -qE "$2"; then
		echo "$1: ok"
	else
		echo "$1: FAIL ($3)"
		status=1
	fi
}

# Counts a guest file's lines matching a pattern.
count_log() {
	guest "grep -cE '$2' $1" | tail -1
}

# Waits (at most some seconds) until a guest file has at least a number of lines matching a pattern.
wait_count() {
	tries=0
	while [ $tries -lt "$4" ]; do
		have=$(count_log "$1" "$2")
		[ "${have:-0}" -ge "$3" ] 2>/dev/null && return 0
		tries=$((tries + 1))
		sleep 1
	done
	echo "wait: $3 of $2 MISSING (${have:-0})"
	status=1
	return 1
}

# The styles a screen last took ("KWL GREETER styles=N").
last_styles() {
	guest "grep -E 'KWL GREETER styles=' $1 | tail -1" | tail -1
}

# Stops sessiond, its greeter and any session.
# By the command (argv[0], such as /bin/wayland), not by the command line:
# since BUG-274 ps -o args shows whole lines, and the guest shell running
# this one names sessiond in its own (/sbin/sessiond, sessiond.log), so a
# match on the line killed that shell before the rest of the command ran.
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,comm | awk "\$2 ~ /(^|\\/)(sessiond|wayland)\$/ {print \$1}"); do kill $p; done; sleep 2'

# 0. Clean logs, no /etc/passkey, the autologin emptied, and the mock's PIN file in kei's home.
guest "$stop_all
[ -f /tmp/passkey-autologin.saved ] || cp /etc/keiland/autologin /tmp/passkey-autologin.saved; : > /etc/keiland/autologin
rm -f /var/log/sessiond.log /var/log/greeter.log $session /etc/passkey" >/dev/null
home=$(guest 'grep "^kei:" /etc/passwd | cut -d: -f6' | tail -1)
guest "mkdir -p $home/.config/keiland && echo old > $home/.config/keiland/pin && chown -R kei $home/.config" >/dev/null
expect passkey-mode '^-r-x------ .* root ' "$(guest 'ls -l /sbin/passkey' | tail -1)"

# 1. The greeter: the password only; kei logs in with it; the old PIN file goes.
guest "/sbin/sessiond --graphical </dev/null >/dev/null 2>&1 & sleep 1; echo started" >/dev/null
expect_log /var/log/greeter.log 'KWL GREETER open .*selected=kei' 60
expect_log /var/log/greeter.log 'KWL GREETER styles=1$' 10
sleep 2
check "$out/greeter.png" >/dev/null
keys 'kei' '\n'
expect_log /var/log/sessiond.log 'SESSIOND AUTH ok user=kei uid=1000 style=password' 10
expect_log $session 'KWL HANDOFF go=1' 20
expect_log $session 'removed the old PIN file' 10
expect old-pin-file 'gone' "$(guest "[ -e $home/.config/keiland/pin ] && echo there || echo gone" | tail -1)"

# 2. kei's PIN, enrolled through passkey with kei's password.
expect enroll '^ok uid=1000' "$(guest "printf 'enroll-pin\nkei\nkei\n246810\n' | /sbin/passkey" | tail -1)"
expect passkey-file '^-rw------- .* root ' "$(guest 'ls -l /etc/passkey' | tail -1)"
expect passkey-line '^1$' "$(guest "grep -c '^kei:1000:pin:\\\$6\\\$' /etc/passkey" | tail -1)"

# 3. The lock screen offers the PIN, and the PIN unlocks.
keys '<super-l>'
expect_log $session 'KWL LOCK locked reason=key user=kei' 5
expect_log $session 'KWL GREETER styles=3' 5
pointer move 1270 790 sleep 300
check "$out/locked.png" >/dev/null
keys '246810' '\n'
expect_log /var/log/sessiond.log 'SESSIOND UNLOCK ok user=kei style=pin' 8
expect_log $session 'KWL LOCK unlocked$' 5

# 4. Five wrong PINs, each waited for, turn the PIN off; the password unlocks and gives it back.
keys '<super-l>'
expect_log $session 'KWL LOCK locked reason=key user=kei' 5
for wrong in 1 2 3 4 5; do
	keys '000000' '\n'
	expect_log /var/log/sessiond.log "SESSIOND UNLOCK fail user=kei wrong=$wrong .*style=pin" 20
	wait_count $session 'KWL GREETER answer=FAIL reason=bad-secret' $wrong 20
	sleep 1
done
sleep 2
expect pin-off 'styles=1$' "$(last_styles $session)"
pointer move 1270 790 sleep 300
check "$out/off.png" >/dev/null
keys 'kei' '\n'
expect_log /var/log/sessiond.log 'SESSIOND UNLOCK ok user=kei style=password' 25
keys '<super-l>'
sleep 2
expect pin-back 'styles=3$' "$(last_styles $session)"
keys '246810' '\n'
sleep 3
pins=$(count_log /var/log/sessiond.log 'SESSIOND UNLOCK ok user=kei style=pin')
expect pin-unlocks-again '^2$' "$pins"

# 5. The session ends: the new greeter offers the PIN, which logs in.
guest 'for p in $(ps -A -o pid,args | grep -E "[w]ayland( |$)" | grep -v -- --greeter | awk "{print \$1}"); do kill $p; done; echo killed' >/dev/null
sleep 6
expect_log /var/log/greeter.log 'KWL GREETER styles=3$' 20
check "$out/greeter-pin.png" >/dev/null
keys '246810' '\n'
expect_log /var/log/sessiond.log 'SESSIOND AUTH ok user=kei uid=1000 style=pin' 10

# 6. sessiond restarted: the password only, until kei logs in with it.
guest "$stop_all; rm -f /var/log/greeter.log; /sbin/sessiond --graphical </dev/null >/dev/null 2>&1 & sleep 1; echo started" >/dev/null
expect_log /var/log/greeter.log 'KWL GREETER open .*selected=kei' 60
expect_log /var/log/greeter.log 'KWL GREETER styles=1$' 10
greeter_pin=$(count_log /var/log/greeter.log 'KWL GREETER styles=3')
expect no-pin-after-restart '^0$' "$greeter_pin"
keys 'kei' '\n'
expect_log /var/log/sessiond.log 'SESSIOND AUTH ok user=kei uid=1000 style=password' 10

# 7. account-admin takes pk2's PIN out at a reset and at the removal.
expect pk2-add 'status=0' "$(guest "/bin/su kei -c 'printf \"kei\nadd\npk2\nPK Two\npasskey123\nuser\n\" | $admin; echo status=\$?'" | tail -1)"
expect pk2-enroll '^ok uid=' "$(guest "printf 'enroll-pin\npk2\npasskey123\n135790\n' | /sbin/passkey" | tail -1)"
expect pk2-line '^1$' "$(guest "grep -c '^pk2:' /etc/passkey" | tail -1)"
expect pk2-reset 'status=0' "$(guest "/bin/su kei -c 'printf \"kei\nreset-password\npk2\npasskey456\n\" | $admin; echo status=\$?'" | tail -1)"
expect pk2-line-after-reset '^0$' "$(guest "grep -c '^pk2:' /etc/passkey" | tail -1)"
expect pk2-enroll-again '^ok uid=' "$(guest "printf 'enroll-pin\npk2\npasskey456\n135790\n' | /sbin/passkey" | tail -1)"
expect pk2-remove 'status=0' "$(guest "/bin/su kei -c 'printf \"kei\nremove\npk2\nremove-home\n\" | $admin; echo status=\$?'" | tail -1)"
expect pk2-line-after-remove '^0$' "$(guest "grep -c '^pk2:' /etc/passkey" | tail -1)"
expect kei-line-kept '^1$' "$(guest "grep -c '^kei:1000:pin:' /etc/passkey" | tail -1)"

# 8. No log has a PIN or a password.
leaks=$(guest "cat /var/log/sessiond.log /var/log/greeter.log $session /var/log/messages 2>/dev/null | grep -cE '246810|135790|passkey123|passkey456'" | tail -1)
expect no-secret-in-logs '^0$' "$leaks"

# The autologin as it was, /etc/passkey removed (kei's session keeps running).
guest "rm -f /etc/passkey; [ -f /tmp/passkey-autologin.saved ] && cat /tmp/passkey-autologin.saved > /etc/keiland/autologin && rm -f /tmp/passkey-autologin.saved" >/dev/null
guest "cat /var/log/sessiond.log" > "$out/sessiond.log"
guest "cat $session" > "$out/session.log"
guest "cat /var/log/greeter.log" > "$out/greeter.log"
echo "passkey-p002-guest: status=$status"
exit $status
