#!/bin/sh
# The default configurations with zedBSD's own ssh, sshd, ssh-keygen and sftp-server in the place of OpenSSH (the
# configurations plan/ws336/tests/config-<board>-ssh.mk: config/ci/config-<board>.mk with openssh swapped for ours),
# in one script: build, boot, check, collect, sum up.
#  1. amd64: the default image (config-amd64-ci-ssh.mk, plan/tools/guest/test-image.sh with the guest harness) and the
#     TCP guest image (config-amd64-tcp-ssh.mk); neither build prints a compiler warning.
#  2. The default image under plan/tools/guest/guest.sh: start, wait, run (the server is zedBSD's sshd, no OpenSSH
#     program is in the image, the service runs it), put and get of 1 MiB back the same.
#  3. plan/tools/ssh-rtt/qemu-ssh-rtt.sh on the default image and plan/tools/tcp-close/qemu-close.sh on the TCP image
#     (their last lines PASS); the login lines ssh-rtt counts ("Accepted publickey") are there.
#  4. The default configurations of the other architectures (REPLACE_BOARDS, default rpi4 rpi5 odroid-c5
#     visionfive2-keiland zybo): their image (disk-image; the ZYBO's root image and SD image) builds with no compiler
#     warning, holds our four programs and libcrypto-compat, and no OpenSSH program.
# Judged over SSH and by the scripts' last lines, never the console.  Expected values are the test's own data.
#   sh plan/ws336/tests/replace-check.sh
# REPLACE_BUILD=<prefix> (default build/p024) names the BUILDs: <prefix>-amd64, <prefix>-tcp, <prefix>-<board>.
# Last line: replace-check: PASS or replace-check: FAIL.  Output under build/tmp/replace-check (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
root=$(pwd -P)
. plan/tools/fresh-out.sh
fresh_out build/tmp/replace-check
out=$(cd build/tmp/replace-check && pwd -P)
prefix=${REPLACE_BUILD:-build/p024}
boards=${REPLACE_BOARDS-rpi4 rpi5 odroid-c5 visionfive2-keiland zybo}
python=/usr/bin/python3
checks=0
failed=0

check() {
	checks=$((checks + 1))
	if [ "$2" = 0 ]; then
		echo "ok $1"
	else
		echo "FAIL $1"
		failed=$((failed + 1))
	fi
}

expect_eq() {
	if [ "$2" = "$3" ]; then check "$1" 0; else check "$1 (got '$2', expected '$3')" 1; fi
}

# warnings LOG: the number of compiler warnings in a build's output.
warnings() {
	grep -cE '^[^ ]+:[0-9]+:([0-9]+:)? warning: ' "$1"
}

# --- 1. the amd64 images.
timeout 7000 sh plan/tools/guest/test-image.sh plan/ws336/tests/config-amd64-ci-ssh.mk "$prefix-amd64" > "$out/image-amd64.log" 2>&1
check "amd64: the default image builds" $?
expect_eq "amd64: no compiler warning" "$(warnings "$out/image-amd64.log")" 0
timeout 7000 sh plan/tools/guest/test-image.sh plan/ws336/tests/config-amd64-tcp-ssh.mk "$prefix-tcp" > "$out/image-tcp.log" 2>&1
check "amd64: the TCP guest image builds" $?
expect_eq "amd64: the TCP image has no compiler warning" "$(warnings "$out/image-tcp.log")" 0
sync

# --- 2. guest.sh on the default image.
GUEST_RUNTIME=$out/guest
export GUEST_RUNTIME
guest() { timeout "${T:-120}" sh plan/tools/guest/guest.sh "$@"; }
guest start --disk nvme "$prefix-amd64/hdd-image.img" > "$out/start.txt" 2>&1
check "guest.sh start" $?
T=400 guest wait --timeout 360 > "$out/wait.txt" 2>&1
check "guest.sh wait" $?
expect_eq "guest.sh run" "$(guest run 'echo run-ok; id -u' 2> /dev/null | tr -d '\r' | tr '\n' ' ')" "run-ok 0 "
port=$($python -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["ssh_port"])' "$GUEST_RUNTIME/session.json")
timeout 60 ssh -v -F /dev/null -p "$port" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes \
    -o IdentitiesOnly=yes -i "$root/plan/tmp/guest/id_ed25519" root@127.0.0.1 true > /dev/null 2> "$out/ident.err"
grep -q 'remote software version zedBSD_1.0' "$out/ident.err"
check "the default image's server is zedBSD's sshd" $?
guest run 'test -x /usr/sbin/sshd && test -x /usr/bin/ssh && test -x /usr/bin/ssh-keygen && test -x /usr/libexec/sftp-server && test -f /lib/libcrypto-compat.so && test ! -e /usr/libexec/sshd-session && test ! -e /usr/bin/scp && test -f /etc/service.d/sshd && test -x /usr/libexec/sshd-start' \
    > /dev/null 2>&1
check "the default image has our programs, the service and its script, and no OpenSSH program" $?
dd if=/dev/urandom of="$out/put.bin" bs=65536 count=16 2> /dev/null
guest put "$out/put.bin" /tmp/put.bin > "$out/put.txt" 2>&1
check "guest.sh put" $?
guest get /tmp/put.bin "$out/get.bin" > "$out/get.txt" 2>&1
check "guest.sh get" $?
cmp -s "$out/put.bin" "$out/get.bin"
check "guest.sh put and get: 1 MiB back the same" $?
guest stop > /dev/null 2>&1
unset GUEST_RUNTIME

# --- 3. the SSH round trips and the close of TCP pairs.
timeout 1800 sh plan/tools/ssh-rtt/qemu-ssh-rtt.sh "$prefix-amd64/hdd-image.img" "$out/ssh-rtt" > "$out/ssh-rtt.log" 2>&1
expect_eq "qemu-ssh-rtt.sh" "$(tail -1 "$out/ssh-rtt.log")" "qemu-ssh-rtt: PASS"
accepted=$(sed -n 's/^log: \([0-9]*\) logins accepted in .*(expected \([0-9]*\)).*/\1 \2/p' "$out/ssh-rtt.log")
[ -n "$accepted" ] && [ "${accepted% *}" -ge "${accepted#* }" ]
check "qemu-ssh-rtt.sh: every login is logged as \"Accepted publickey\" ($accepted)" $?
timeout 1800 sh plan/tools/tcp-close/qemu-close.sh "$prefix-tcp/hdd-image.img" "$out/tcp-close" > "$out/tcp-close.log" 2>&1
expect_eq "qemu-close.sh" "$(tail -1 "$out/tcp-close.log")" "qemu-close: PASS"
sync

# --- 4. the other architectures' default configurations.
for board in $boards; do
	build=$prefix-$board
	case "$board" in
	zybo) targets="$build/root.img sd-image" ;;
	*) targets="disk-image" ;;
	esac
	# shellcheck disable=SC2086
	yes | timeout 7000 make -j16 ZEDBSD_CONFIG="plan/ws336/tests/config-$board-ssh.mk" BUILD="$build" firmware-licenses \
	    > "$out/image-$board.log" 2>&1
	# shellcheck disable=SC2086
	timeout 7000 make -j16 ZEDBSD_CONFIG="plan/ws336/tests/config-$board-ssh.mk" BUILD="$build" $targets \
	    >> "$out/image-$board.log" 2>&1
	check "$board: the image builds" $?
	expect_eq "$board: no compiler warning" "$(warnings "$out/image-$board.log")" 0
	missing=""
	for file in usr/bin/ssh usr/sbin/sshd usr/bin/ssh-keygen usr/libexec/sftp-server lib/libcrypto-compat.so \
	    etc/ssh/sshd_config etc/service.d/sshd usr/libexec/sshd-start; do
		[ -f "$build/rootfs/$file" ] || missing="$missing $file"
	done
	expect_eq "$board: the root holds our programs, libcrypto-compat and the server's files" "$missing" ""
	[ ! -e "$build/rootfs/usr/libexec/sshd-session" ] && [ ! -e "$build/rootfs/usr/bin/scp" ]
	check "$board: no OpenSSH program in the root" $?
	sync
done

sync
echo "replace-check: $checks checks, $failed failed"
if [ "$failed" = 0 ] && [ "$checks" -gt 0 ]; then echo "replace-check: PASS"; else echo "replace-check: FAIL"; fi
[ "$failed" = 0 ]
