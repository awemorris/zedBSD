#!/bin/sh
# ws172-p003: the security key style without a key (the user's decision: the key's own flow is the real YubiKey's UAT,
# WS161 p006), on the guest of plan/ws172/tests/build-fido2-image.sh, run like passkey-p002-guest.sh: the boot's
# autologin session is stopped, the autologin emptied (put back at the end) and sessiond started, so the greeter comes
# up with kei (password "kei") selected.
#  0. /usr/libexec/passkey-fido2 is root's alone (-r-x------); the _passkey account is there.
#  1. kei gets a key's line in /etc/passkey (root's, 0600; a made-up credential: no key holds it).
#  2. The greeter offers the password and the key (KWL GREETER styles=5): greeter.png.  A click on its pill under the
#     field takes the key (style=4, "Security key PIN"): key.png.  A PIN of three is not sent ("at least four").
#  3. A PIN of four is sent: passkey-fido2 finds no key that holds the credential (the test kernel's loopback key does
#     not speak CTAP2).  With no reader it fails at once (reason=no-key); with a card reader's slot (the image's) it
#     says "touch" and waits for a card held to the reader for the touch's time, then fails (reason=timeout:
#     ws199-p001 section 5 and R3, the card tapped during the attempt).  Either way SESSIOND AUTH fail and the
#     greeter's answer=FAIL say so: no-key.png.
#  4. kei logs in with the password; Settings' Security Keys page (ws199-p001) lists the key: settings.png.
#  5. No log has a PIN or a password.
#
#   plan/ws172/tests/build-fido2-image.sh BUILD; plan/tools/files/files-guest.sh start BUILD/hdd-image.img
#   plan/tools/guest/guest.py wait; sleep 30   (the boot's own sessiond settled: one sessiond only)
#   plan/ws172/tests/fido2-p003-guest.sh [OUTDIR]       (default build/ws172-fido2)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
GUEST_RUNTIME="${GUEST_RUNTIME:-$(pwd)/build/ws071-run}"
export GUEST_RUNTIME
out=${1:-build/ws172-fido2}
mkdir -p "$out"
guest() { timeout 120 python3 plan/tools/guest/guest.py run "$1" 2>&1 </dev/null; }
check() { python3 plan/ws035/tests/zdesktop-check.py "$@" --runtime "$GUEST_RUNTIME"; }
keys() { python3 plan/ws035/tests/qmp-keys.py "$GUEST_RUNTIME/qmp.sock" "$@"; sleep 0.8; }
pointer() { python3 plan/ws035/tests/qmp-pointer.py --width 1280 --height 800 "$GUEST_RUNTIME/qmp.sock" "$@"; }
status=0
session=/run/user/1000/session.log

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

# Stops sessiond, its greeter and any session.
# By the command (argv[0], such as /bin/wayland), not by the command line:
# since BUG-274 ps -o args shows whole lines, and the guest shell running
# this one names sessiond in its own (/sbin/sessiond, sessiond.log), so a
# match on the line killed that shell before the rest of the command ran.
stop_all='service stop greeter >/dev/null 2>&1; for p in $(ps -A -o pid,comm | awk "\$2 ~ /(^|\\/)(sessiond|wayland)\$/ {print \$1}"); do kill $p; done; sleep 2'

# 0. The program and the account; the logs emptied, the autologin emptied.  kei has taken Settings' Welcome already
#    (welcome.done=1 in ~/.config/keiland/desktop.conf): its first session would otherwise open the Welcome, and the
#    Users page asked for in 4 would go to that window (T1-278).
guest "$stop_all
[ -f /tmp/fido2-autologin.saved ] || cp /etc/keiland/autologin /tmp/fido2-autologin.saved; : > /etc/keiland/autologin
: > /var/log/sessiond.log; : > /var/log/greeter.log
conf=/home/kei/.config/keiland/desktop.conf; mkdir -p /home/kei/.config/keiland; touch \$conf
grep -v '^welcome.done=' \$conf > /tmp/fido2-desktop.conf; echo welcome.done=1 >> /tmp/fido2-desktop.conf; cat /tmp/fido2-desktop.conf > \$conf
chown -R kei /home/kei/.config" >/dev/null
expect welcome-done '^welcome.done=1$' "$(guest 'grep "^welcome.done=" /home/kei/.config/keiland/desktop.conf' | tail -1)"
expect fido2-mode '^-r-x------ .* root ' "$(guest 'ls -l /usr/libexec/passkey-fido2' | tail -1)"
expect passkey-account '^_passkey:x:79:79:' "$(guest 'grep "^_passkey:" /etc/passwd' | tail -1)"

# 1. kei's made-up key (its ID AQIDBA, a P-256 key no key holds).
guest "umask 077; printf '# zedBSD passkey 1\nkei:1000:fido2:AQIDBA:pQECAyYgASFYIGBhYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5ent8fX5_IlggQEFCQ0RFRkdISUpLTE1OT1BRUlNUVVZXWFlaW1xdXl8:0:zedbsd.login:Desk key:2026-10-06\n' > /etc/passkey; chown root /etc/passkey; chmod 600 /etc/passkey" >/dev/null
expect passkey-file '^-rw------- .* root ' "$(guest 'ls -l /etc/passkey' | tail -1)"

# 2. The greeter offers the key; the link takes it.
guest "/sbin/sessiond --graphical </dev/null >/dev/null 2>&1 & sleep 1; echo started" >/dev/null
expect_log /var/log/greeter.log 'KWL GREETER open .*selected=kei' 60
expect_log /var/log/greeter.log 'KWL GREETER styles=5$' 10
sleep 2
check "$out/greeter.png" >/dev/null
# The styles side by side under the field (ws172-p007): the middle of a style's pill ("KWL GREETER style-at style=N").
style_at() {
	guest "grep -E 'KWL GREETER style-at style=$1 ' /var/log/greeter.log | tail -1" | tail -1 |
	    sed -n 's/.* x=\([0-9]*\) y=\([0-9]*\) width=\([0-9]*\) height=\([0-9]*\).*/\1 \2 \3 \4/p' | awk '{print $1 + $3 / 2, $2 + $4 / 2}'
}
set -- $(style_at 4)
pointer move "${1:-700}" "${2:-500}" down sleep 60 up
expect_log /var/log/greeter.log 'KWL GREETER style=4( via=choice)?$' 5
pointer move 1270 790 sleep 300
check "$out/key.png" >/dev/null
keys '123' '\n'
sleep 1
expect short-pin '^0$' "$(guest "grep -c 'KWL GREETER auth user=kei style=4' /var/log/greeter.log" | tail -1)"

# 3. A PIN of four goes; no key holds the credential.
keys '<backspace>' '<backspace>' '<backspace>' '4321' '\n'
expect_log /var/log/greeter.log 'KWL GREETER auth user=kei style=4' 5
expect_log /var/log/sessiond.log 'SESSIOND AUTH fail user=kei .*reason=(no-key|timeout)' 100
expect_log /var/log/greeter.log 'KWL GREETER answer=FAIL reason=(no-key|timeout)' 10
pointer move 1270 790 sleep 300
check "$out/no-key.png" >/dev/null

# 4. The password logs in; Settings' Security Keys page lists the key (ws199-p001).
set -- $(style_at 1)
pointer move "${1:-580}" "${2:-500}" down sleep 60 up
expect_log /var/log/greeter.log 'KWL GREETER style=1( via=choice)?$' 5
keys 'kei' '\n'
expect_log /var/log/sessiond.log 'SESSIOND AUTH ok user=kei uid=1000 style=password' 10
expect_log $session 'KWL HANDOFF go=1' 20
expect_log $session 'KWL WELCOME skip done=1 error=0' 10
# Settings on the session's own socket (its compositor's KWL READY line names it; T1-309: wayland-0 was not it), then
# the key's line the Security Keys page asks for.
socket=$(guest "grep -o 'KWL READY socket=[^ ]*' $session | tail -1 | sed 's/KWL READY socket=//'" | tail -1)
socket=${socket:-/run/user/1000/wayland-0}
echo "session socket: $socket"
guest "su kei -c 'XDG_RUNTIME_DIR=$(dirname "$socket") WAYLAND_DISPLAY=$(basename "$socket") /bin/settings security-keys >/tmp/fido2-settings.log 2>&1 &'; sleep 4; echo started" >/dev/null
expect_log /tmp/fido2-settings.log 'ZSETTINGS PAGE security-keys' 20
expect_log $session 'KWL SYSTEM enrolled pin=0 keys=1 listed=1' 20
guest "tail -5 /tmp/fido2-settings.log" | sed 's/^/settings: /'
sleep 2
check "$out/settings.png" >/dev/null

# 5. No secret in a log.
expect no-secret '^0$' "$(guest "cat /var/log/sessiond.log /var/log/greeter.log $session | grep -c 4321" | tail -1)"

# The autologin as it was; /etc/passkey emptied (kei's session keeps running).
guest ": > /etc/passkey; [ -f /tmp/fido2-autologin.saved ] && cat /tmp/fido2-autologin.saved > /etc/keiland/autologin" >/dev/null
[ $status = 0 ] && echo "fido2-p003-guest: PASS" || echo "fido2-p003-guest: FAIL"
exit $status
