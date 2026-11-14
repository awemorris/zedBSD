#!/bin/sh
# zedBSD's own sshd and ssh (userland/base/ssh) on a zedBSD guest in QEMU, against the host's OpenSSH: one script that
# builds the images, boots them, runs the checks, collects what they printed and sums up.
#  1. Images from plan/tools/ssh/config-amd64-ssh.mk (OpenSSH and OpenSSL out, ours in) with
#     plan/tools/guest/test-image.sh: "keyed" with the guest harness's files (root's authorized_keys, net.conf and the
#     host keys), "fresh" with only root's authorized_keys and net.conf, so the guest makes its own host key.  Neither
#     build may print a compiler warning.  plan/tools/boot-test.sh photographs the keyed image at its login prompt.
#  2. The keyed guest (plan/tools/guest/guest.py, KVM, NVMe, its own runtime directory and ports; the forwarded port
#     is checked to be this test's QEMU), from the host's OpenSSH ssh, scp and sftp to the guest's sshd running as
#     root: the server is ours (its identification line, mlkem768x25519-sha256, the harness's ssh-ed25519 host key, no
#     OpenSSH program in the image); root by key; user and root by password (the shadow file); a wrong password and an
#     unknown user refused; scp both ways of 8 MiB (SFTP); sftp -b mkdir, put, rename, ls, get; three sessions over one
#     ControlMaster connection; -tt with the terminal's size, on a pseudo-terminal that tty names, with the login record written
#     while the session lasts (who) and closed after it; 64 MiB both ways with RekeyLimit 1M, the re-exchanges seen.
#  3. Privilege separation in the guest: a connection held before its key exchange has a pre-authentication process
#     (the -Zpreauth sshd) of uid 22 whose root directory is /var/empty (fuser), under the monitor of uid 0; a user's
#     session has a -Zsession sshd of the user's uid.
#  4. From the guest's ssh: to the host's OpenSSH sshd (this user's, on 127.0.0.1 with the test's configuration and
#     keys, reached from the guest as 10.0.2.2, checked to be the test's process) with a key the guest's ssh-keygen made
#     (output, exit status, 1 MiB through and back), and to the guest's own sshd.
#  5. The fresh guest: sshd-start made /etc/ssh/ssh_host_ed25519_key (0600) at its first start and nothing else, the
#     server offers that key, and it is not the harness's.
# Expected values are the test's own data, the account files of the tree (user and root have their names as
# passwords, uid 1000 and 0, sshd is uid 22), the formats of the programs' output and the protocol's names; OpenSSH is
# the peer, never the source of an expected value.  Nothing outside the run directory and the BUILD is written; the
# host's sshd runs as this user with ForceCommand, so no interactive login shell runs on the host.
#   sh plan/tools/ssh/qemu-interop.sh
# QEMU_INTEROP_BUILD=<dir> chooses the BUILD (default build/ssh-interop).  QEMU_INTEROP_PARTS="boot transfer fresh" (a part of
# that list) keeps only those of the optional parts: the boot test, scp/sftp/ControlMaster/re-key, and the fresh image;
# the rest always runs.  Last line: qemu-interop: PASS or qemu-interop: FAIL.
# Output under build/tmp/qemu-interop (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
root=$(pwd -P)
. plan/tools/fresh-out.sh
fresh_out build/tmp/qemu-interop
out=$(cd build/tmp/qemu-interop && pwd -P)
python=/usr/bin/python3
helper="$python -I $root/plan/tools/ssh/qemu-helper.py"
guest="$python -I $root/plan/tools/guest/guest.py"
build=${QEMU_INTEROP_BUILD:-build/ssh-interop}
config=plan/tools/ssh/config-amd64-ssh.mk
key=$root/plan/tmp/guest/id_ed25519
harness_host_key=$root/plan/tmp/guest/ssh_host_ed25519_key.pub
checks=0
failed=0
host_pid=""
mkdir -p "$out/work"
work=$out/work

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

# want PART: whether PART (boot, transfer, fresh) is in QEMU_INTEROP_PARTS (default all).
want() {
	case " ${QEMU_INTEROP_PARTS:-all} " in
	*" all "*|*" $1 "*) return 0 ;;
	esac
	return 1
}

finish() {
	[ -n "$host_pid" ] && kill "$host_pid" 2> /dev/null
	GUEST_RUNTIME=$out/keyed $guest stop > /dev/null 2>&1
	GUEST_RUNTIME=$out/fresh $guest stop > /dev/null 2>&1
	sync
	echo "qemu-interop: $checks checks, $failed failed"
	if [ "$failed" = 0 ] && [ "$checks" -gt 0 ]; then echo "qemu-interop: PASS"; else echo "qemu-interop: FAIL"; fi
	[ "$failed" = 0 ]
	exit $?
}

# The options of every connection from the host's OpenSSH to the guest; KNOWN is the known_hosts file of the run.
port=0
host_options() {
	echo "-F /dev/null -p $port -o UserKnownHostsFile=$known -o GlobalKnownHostsFile=/dev/null -o StrictHostKeyChecking=accept-new -o LogLevel=ERROR -o ConnectTimeout=20 -o ControlMaster=no -o ControlPath=none"
}
# g_root COMMAND: runs COMMAND in the guest as root, with the harness key.
g_root() {
	# shellcheck disable=SC2046
	timeout "${T:-120}" ssh $(host_options) -o BatchMode=yes -o IdentitiesOnly=yes -i "$key" root@127.0.0.1 "$@"
}
# g_password USER PASSWORD COMMAND: logs in as USER with PASSWORD only.
g_password() {
	who=$1
	secret=$2
	shift 2
	# shellcheck disable=SC2046
	timeout "${T:-120}" sshpass -p "$secret" ssh $(host_options) -o PubkeyAuthentication=no \
	    -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1 "$who@127.0.0.1" "$@"
}

# boot NAME IMAGE: starts a guest from IMAGE with its own runtime directory and waits for its sshd; sets port.
boot() {
	GUEST_RUNTIME=$out/$1 timeout 60 $guest start --disk nvme "$2" > "$work/$1-start.out" 2>&1 || return 1
	GUEST_RUNTIME=$out/$1 timeout 400 $guest wait --timeout 360 > "$work/$1-wait.out" 2>&1 || return 1
	port=$($python -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["ssh_port"])' "$out/$1/session.json")
	pid=$($python -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["pid"])' "$out/$1/session.json")
	$helper owner "$port" "$pid"
}

# --- 1. the images.  The image's files are copies made now, so each image is packed again: the cached root tree of
# the BUILD is remade only when one of its files is newer than it.
mkdir -p "$work/keyed-files"
cp "$key.pub" "$work/keyed-files/authorized_keys"
cp "$root/plan/tools/guest/net.conf" "$work/keyed-files/net.conf"
keyed_files="--file /root/.ssh/authorized_keys=$work/keyed-files/authorized_keys --file /etc/net.conf=$work/keyed-files/net.conf"
keyed_files="$keyed_files --mode /root/.ssh/authorized_keys=0600 --mode /root/.ssh=0700"
for kind in rsa ecdsa ed25519; do
	cp "$root/plan/tmp/guest/ssh_host_${kind}_key" "$root/plan/tmp/guest/ssh_host_${kind}_key.pub" "$work/keyed-files/"
	keyed_files="$keyed_files --file /etc/ssh/ssh_host_${kind}_key=$work/keyed-files/ssh_host_${kind}_key"
	keyed_files="$keyed_files --file /etc/ssh/ssh_host_${kind}_key.pub=$work/keyed-files/ssh_host_${kind}_key.pub"
	keyed_files="$keyed_files --mode /etc/ssh/ssh_host_${kind}_key=0600"
done
# shellcheck disable=SC2086
timeout 5400 sh plan/tools/guest/test-image.sh --no-harness "$config" "$build" $keyed_files > "$out/image-keyed.log" 2>&1
check "the keyed image builds" $?
expect_eq "the keyed image's build has no compiler warning" "$(grep -cE '^[^ ]+:[0-9]+:([0-9]+:)? warning: ' "$out/image-keyed.log")" 0
cp "$build/hdd-image.img" "$out/keyed.img" || check "the keyed image is there" 1
if want fresh; then
	cp "$key.pub" "$work/authorized_keys"
	cp "$root/plan/tools/guest/net.conf" "$work/net.conf"
	timeout 5400 sh plan/tools/guest/test-image.sh --no-harness "$config" "$build" \
	    --file /root/.ssh/authorized_keys="$work/authorized_keys" --file /etc/net.conf="$work/net.conf" \
	    --mode /root/.ssh/authorized_keys=0600 --mode /root/.ssh=0700 > "$out/image-fresh.log" 2>&1
	check "the fresh image builds" $?
	expect_eq "the fresh image's build has no compiler warning" "$(grep -cE '^[^ ]+:[0-9]+:([0-9]+:)? warning: ' "$out/image-fresh.log")" 0
	cp "$build/hdd-image.img" "$out/fresh.img" || check "the fresh image is there" 1
fi
sync
if want boot; then
	OUTPUT=$out/boot-test BOOT_TEST_WORK=$out/boot-work BOOT_TIMEOUT=300 timeout 420 bash plan/tools/boot-test.sh "$out/keyed.img" \
	    > "$out/boot-test.log" 2>&1
	check "boot-test: the keyed image reaches its login prompt" $?
fi

# --- 2. the keyed guest from the host's OpenSSH.
known=$work/known_keyed
if ! boot keyed "$out/keyed.img"; then
	check "the keyed guest answers on SSH (its own forwarded port)" 1
	finish
fi
check "the keyed guest answers on SSH (its own forwarded port)" 0
# shellcheck disable=SC2046
timeout 60 ssh $(host_options) -v -o BatchMode=yes -o IdentitiesOnly=yes -i "$key" root@127.0.0.1 'id -u' \
    > "$work/root-key.out" 2> "$work/root-key.err"
expect_eq "root logs in with the key" "$(cat "$work/root-key.out")" 0
grep -q 'remote software version zedBSD_1.0' "$work/root-key.err"
check "the guest's server is zedBSD's sshd" $?
grep -q 'kex: algorithm: mlkem768x25519-sha256' "$work/root-key.err"
check "the key exchange is mlkem768x25519-sha256" $?
grep -q 'kex: host key algorithm: ssh-ed25519' "$work/root-key.err"
check "the host key is ssh-ed25519" $?
expect_eq "the host key is the harness's" "$(awk '{ print $3 }' "$known" | head -1)" "$(cut -d ' ' -f 2 "$harness_host_key")"
g_root 'test -f /etc/ssh/ssh_host_rsa_key && test -f /etc/ssh/ssh_host_ecdsa_key'
check "the keyed guest has the harness's host keys (the image was packed with them)" $?
g_root 'test -x /usr/sbin/sshd && test -x /usr/bin/ssh && test -x /usr/libexec/sftp-server && test ! -e /usr/libexec/sshd-session && test ! -e /usr/bin/scp'
check "the image has our programs and no OpenSSH" $?

expect_eq "user logs in with the password" "$(g_password user user 'id -u; id -un' 2> "$work/user-pw.err" | tr '\n' ' ')" "1000 user "
expect_eq "root logs in with the password (PermitRootLogin yes)" "$(g_password root root 'id -u' 2> "$work/root-pw.err")" 0
g_password user wrong 'id -u' > "$work/wrong.out" 2> "$work/wrong.err"
rc=$?
[ "$rc" != 0 ] && [ ! -s "$work/wrong.out" ] && grep -q 'Permission denied' "$work/wrong.err"
check "a wrong password is refused (status $rc)" $?
g_password nosuchuser nosuchuser 'id -u' > "$work/nouser.out" 2> "$work/nouser.err"
rc=$?
[ "$rc" != 0 ] && [ ! -s "$work/nouser.out" ] && grep -q 'Permission denied' "$work/nouser.err"
check "an unknown user is refused (status $rc)" $?

if want transfer; then
	# scp (SFTP) both ways and sftp -b.
	dd if=/dev/urandom of="$work/up.bin" bs=1048576 count=8 2> /dev/null
	# shellcheck disable=SC2046
	T=300 timeout 300 scp $(host_options | sed 's/-p /-P /') -o BatchMode=yes -o IdentitiesOnly=yes -i "$key" -q \
	    "$work/up.bin" root@127.0.0.1:/tmp/up.bin
	check "scp of 8 MiB to the guest" $?
	# shellcheck disable=SC2046
	timeout 300 scp $(host_options | sed 's/-p /-P /') -o BatchMode=yes -o IdentitiesOnly=yes -i "$key" -q \
	    root@127.0.0.1:/tmp/up.bin "$work/down.bin"
	check "scp of 8 MiB from the guest" $?
	cmp -s "$work/up.bin" "$work/down.bin"
	check "scp: the 8 MiB came back the same" $?
	printf 'mkdir /tmp/sb\nput %s /tmp/sb/a\nrename /tmp/sb/a /tmp/sb/b\nls /tmp/sb\nget /tmp/sb/b %s\n' \
	    "$work/up.bin" "$work/sftp.bin" > "$work/sftp.batch"
	# shellcheck disable=SC2046
	timeout 300 sftp $(host_options | sed 's/-p /-P /') -o BatchMode=yes -o IdentitiesOnly=yes -i "$key" -b "$work/sftp.batch" \
	    root@127.0.0.1 > "$work/sftp.out" 2>&1
	check "sftp -b: mkdir, put, rename, ls, get" $?
	grep -q '/tmp/sb/b' "$work/sftp.out"
	check "sftp -b: ls shows the renamed file" $?
	cmp -s "$work/up.bin" "$work/sftp.bin"
	check "sftp -b: the file came back the same" $?

	# Three sessions over one ControlMaster connection.
	# shellcheck disable=SC2046
	timeout 60 ssh $(host_options | sed 's/-o ControlMaster=no -o ControlPath=none//') -o BatchMode=yes -o IdentitiesOnly=yes \
	    -i "$key" -o ControlMaster=yes -o ControlPath="$work/cm.sock" -o ControlPersist=120 -f -N root@127.0.0.1
	check "ControlMaster: the master connection" $?
	for n in 1 2 3; do
		# shellcheck disable=SC2046
		timeout 60 ssh $(host_options | sed 's/-o ControlMaster=no -o ControlPath=none//') -o ControlPath="$work/cm.sock" \
		    root@127.0.0.1 "echo session-$n; sleep 2" > "$work/cm-$n.out" 2>&1 &
	done
	wait
	expect_eq "ControlMaster: three sessions over the master" "$(cat "$work/cm-1.out" "$work/cm-2.out" "$work/cm-3.out" | tr '\n' ' ')" \
	    "session-1 session-2 session-3 "
	# shellcheck disable=SC2046
	timeout 30 ssh $(host_options | sed 's/-o ControlMaster=no -o ControlPath=none//') -o ControlPath="$work/cm.sock" -O exit \
	    root@127.0.0.1 > /dev/null 2>&1
fi

# -tt: the terminal's size, a pseudo-terminal, and the login record (who) during the session and not after it.
# shellcheck disable=SC2046
$helper pty "$work/tty.out" 37 123 60 -- /usr/bin/ssh $(host_options) -o BatchMode=yes -o IdentitiesOnly=yes -i "$key" \
    -tt root@127.0.0.1 'printf "SIZE %s\n" "$(stty size)"; if [ -t 0 ]; then echo TERMINAL; fi; printf "TTY %s\n" "$(tty)"; who'
expect_eq "-tt: exit status" "$?" 0
tr -d '\r' < "$work/tty.out" > "$work/tty.txt"
grep -q '^SIZE 37 123$' "$work/tty.txt"
check "-tt: the terminal has the client's size" $?
grep -qx TERMINAL "$work/tty.txt"
check "-tt: the command's standard input is a terminal" $?
tty_name=$(awk '$1 == "root" && $2 ~ /^pts\/[0-9]+$/ { print $2 }' "$work/tty.txt")
[ "$(echo "$tty_name" | grep -c .)" = 1 ]
check "-tt: who shows root on one pseudo-terminal while the session lasts (LOGIN_RECORD: $tty_name)" $?
expect_eq "-tt: tty names the session's pseudo-terminal" "$(sed -n 's/^TTY //p' "$work/tty.txt")" "/dev/$tty_name"
sleep 2
g_root who > "$work/who-after.txt" 2>&1
check "-tt: who runs after the session" $?
if [ -n "$tty_name" ]; then
	grep -q " $tty_name " "$work/who-after.txt"
	expect_eq "-tt: the record of $tty_name is closed after the session" "$?" 1
fi

if want transfer; then
	# 64 MiB both ways with RekeyLimit 1M.
	dd if=/dev/urandom of="$work/big.bin" bs=1048576 count=64 2> /dev/null
	# shellcheck disable=SC2046
	T=600 timeout 600 ssh $(host_options) -v -o RekeyLimit=1M -o BatchMode=yes -o IdentitiesOnly=yes -i "$key" root@127.0.0.1 \
	    'cat > /tmp/big.bin; cat /tmp/big.bin' < "$work/big.bin" > "$work/big-back.bin" 2> "$work/rekey.err"
	check "RekeyLimit 1M: 64 MiB to the guest and back" $?
	cmp -s "$work/big.bin" "$work/big-back.bin"
	check "RekeyLimit 1M: the 64 MiB came back the same" $?
	rekeys=$(grep -c 'SSH2_MSG_NEWKEYS received' "$work/rekey.err")
	[ "$rekeys" -ge 10 ]
	check "RekeyLimit 1M: the keys were exchanged again ($rekeys times)" $?
fi

# --- 3. privilege separation.
$helper hold "$port" 40 > "$work/hold.out" 2>&1 &
hold_pid=$!
for n in 1 2 3 4 5 6 7 8 9 10; do
	grep -q held "$work/hold.out" && break
	sleep 1
done
sleep 2
g_root 'ps -A -o pid,ppid,uid,args' > "$work/ps-preauth.txt" 2>&1
g_root 'fuser -u /var/empty' > "$work/fuser.txt" 2>&1
preauth=$(awk -v a=-Zpre -v b=auth '$4 ~ /sshd$/ && $5 == a b { print $1, $2, $3 }' "$work/ps-preauth.txt")
[ "$(echo "$preauth" | grep -c .)" = 1 ]
check "privsep: the held connection has one pre-authentication process" $?
preauth_pid=$(echo "$preauth" | awk '{ print $1 }')
expect_eq "privsep: the pre-authentication process runs as uid 22 (sshd)" "$(echo "$preauth" | awk '{ print $3 }')" 22
monitor_pid=$(echo "$preauth" | awk '{ print $2 }')
expect_eq "privsep: its parent, the monitor, runs as root" "$(awk -v p="$monitor_pid" '$1 == p { print $3 }' "$work/ps-preauth.txt")" 0
[ -n "$preauth_pid" ] && grep -qE " ${preauth_pid}[a-z]*r[a-z]*\(" "$work/fuser.txt"
check "privsep: its root directory is /var/empty (fuser shows r)" $?
kill "$hold_pid" 2> /dev/null
wait "$hold_pid" 2> /dev/null
T=60 g_password user user 'sleep 20' > /dev/null 2>&1 &
session_client=$!
sleep 8
g_root 'ps -A -o pid,ppid,uid,args' > "$work/ps-session.txt" 2>&1
# Two sessions: the user's, and root's that runs ps.
expect_eq "privsep: each session process runs as its user (user 1000, root 0)" \
    "$(awk -v a=-Zses -v b=sion '$4 ~ /sshd$/ && $5 == a b { print $3 }' "$work/ps-session.txt" | sort -n | tr '\n' ' ')" "0 1000 "
wait "$session_client"

# --- 4. the guest's ssh, to the host's OpenSSH sshd and to the guest's own sshd.
g_root 'mkdir -p /root/.ssh && /usr/bin/ssh-keygen -q -t ed25519 -N "" -C guest -f /root/.ssh/id_ed25519 && cat /root/.ssh/id_ed25519.pub' \
    > "$work/guest-key.pub" 2> "$work/guest-key.err"
grep -q '^ssh-ed25519 ' "$work/guest-key.pub"
check "the guest's ssh-keygen made a key" $?
mkdir -p "$work/server"
ssh-keygen -q -t ed25519 -N '' -C host -f "$work/server/host_key" > /dev/null 2>&1
cp "$work/guest-key.pub" "$work/server/authorized_keys"
host_port=$($python -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
{
	echo "ForceCommand eval \"\$SSH_ORIGINAL_COMMAND\""
	echo "PermitUserRC no"
	echo "PermitUserEnvironment no"
	echo "X11Forwarding no"
	echo "AllowAgentForwarding no"
	echo "AllowTcpForwarding no"
	echo "PermitTunnel no"
	echo "Port $host_port"
	echo "ListenAddress 127.0.0.1"
	echo "HostKey $work/server/host_key"
	echo "AuthorizedKeysFile $work/server/authorized_keys"
	echo "PidFile none"
	echo "UsePAM no"
	echo "StrictModes no"
	echo "PasswordAuthentication no"
	echo "KbdInteractiveAuthentication no"
} > "$work/server/sshd_config"
timeout 900 /usr/sbin/sshd -D -e -f "$work/server/sshd_config" > "$work/server/sshd.log" 2>&1 &
host_pid=$!
$helper wait-port "$host_port" 10 && $helper owner "$host_port" "$host_pid"
check "the host's test sshd listens on 127.0.0.1:$host_port (the test's own process)" $?
me=$(id -un)
g_root "/usr/bin/ssh -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/root/.ssh/known_hosts -o BatchMode=yes -i /root/.ssh/id_ed25519 -p $host_port $me@10.0.2.2 'echo from-guest; exit 7'" \
    > "$work/to-host.out" 2> "$work/to-host.err"
expect_eq "guest ssh to the host's OpenSSH sshd: exit status" "$?" 7
expect_eq "guest ssh to the host's OpenSSH sshd: output" "$(cat "$work/to-host.out")" from-guest
expect_eq "guest ssh: known_hosts holds the host's test key" \
    "$(g_root "awk '{ print \$3 }' /root/.ssh/known_hosts" 2> /dev/null | head -1)" "$(cut -d ' ' -f 2 "$work/server/host_key.pub")"
g_root "dd if=/dev/urandom of=/tmp/r.bin bs=65536 count=16 2> /dev/null && /usr/bin/ssh -o BatchMode=yes -i /root/.ssh/id_ed25519 -p $host_port $me@10.0.2.2 cat < /tmp/r.bin > /tmp/r2.bin && cmp /tmp/r.bin /tmp/r2.bin" \
    > "$work/to-host-data.out" 2>&1
check "guest ssh to the host's OpenSSH sshd: 1 MiB through and back" $?
$helper owner "$host_port" "$host_pid"
check "the host's test sshd is still the test's own process" $?
g_root 'cat /root/.ssh/id_ed25519.pub >> /root/.ssh/authorized_keys && /usr/bin/ssh -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/root/.ssh/known_hosts -o BatchMode=yes -i /root/.ssh/id_ed25519 root@127.0.0.1 "echo self; id -u"' \
    > "$work/to-self.out" 2> "$work/to-self.err"
expect_eq "guest ssh to the guest's own sshd" "$(tr '\n' ' ' < "$work/to-self.out")" "self 0 "
kill "$host_pid" 2> /dev/null
wait "$host_pid" 2> /dev/null
host_pid=""
GUEST_RUNTIME=$out/keyed timeout 60 $guest stop > /dev/null 2>&1

# --- 5. the fresh guest: the host key that sshd-start makes.
if want fresh; then
	known=$work/known_fresh
	if ! boot fresh "$out/fresh.img"; then
		check "the fresh guest answers on SSH (its own forwarded port)" 1
		finish
	fi
	check "the fresh guest answers on SSH (its own forwarded port)" 0
	g_root 'ls /etc/ssh; ls -l /etc/ssh/ssh_host_ed25519_key' > "$work/fresh-ls.txt" 2>&1
	expect_eq "fresh: /etc/ssh holds the config files and the one host key sshd-start made" \
	    "$(sed -n '1,/^-/p' "$work/fresh-ls.txt" | grep -v '^-' | sort | tr '\n' ' ')" \
	    "ssh_config ssh_host_ed25519_key ssh_host_ed25519_key.pub sshd_config "
	grep -q '^-rw------- ' "$work/fresh-ls.txt"
	check "fresh: the host key is readable by root only" $?
	expect_eq "fresh: the server offers the key it made" "$(awk '{ print $3 }' "$known" | head -1)" \
	    "$(g_root "cut -d ' ' -f 2 /etc/ssh/ssh_host_ed25519_key.pub" 2> /dev/null)"
	[ "$(awk '{ print $3 }' "$known" | head -1)" != "$(cut -d ' ' -f 2 "$harness_host_key")" ]
	check "fresh: the key is not the harness's" $?
fi

finish
