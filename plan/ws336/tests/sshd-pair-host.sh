#!/bin/sh
# sshd's three processes (monitor, pre-authentication, session) on a development host without root, against the host's
# OpenSSH ssh and sftp: the driver plan/ws336/tests/sshd-pair-host.c stands in for the listener, takes one connection
# on 127.0.0.1 and runs sshd_monitor_run_program with itself as the children.  Each case is one connection.
# Nothing outside the run directory is touched: the account the test runs as is given the run directory's home and
# shell (getpwnam_r is replaced in the driver), the log goes to a file there, ssh and sftp run with -F /dev/null, their
# own known_hosts file and key, and an empty environment.
#   sh plan/ws336/tests/sshd-pair-host.sh
# Last line: sshd-pair-host: PASS or sshd-pair-host: FAIL.  Output under build/tmp/sshd-pair (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
root=$(pwd)
. plan/tools/fresh-out.sh
fresh_out build/tmp/sshd-pair
work=$(cd build/tmp/sshd-pair && pwd -P)
status=0
user=$(id -un)

fail() {
	echo "FAIL $*"
	status=1
}

# The driver, with every module and sshd's files, ASan and UBSan.
modules=""
for file in userland/base/ssh/ssh-*.c; do
	case "$file" in
	*-main.c|*/ssh-client-*.c) ;;
	*) modules="$modules $file" ;;
	esac
done
if ! timeout 300 cc -std=gnu99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fno-omit-frame-pointer -I. -Iuserland/base/ssh -Iplan/ws336/tests/host-crypto plan/ws336/tests/sshd-pair-host.c \
    $modules userland/base/ssh/sshd-monitor.c userland/base/ssh/sshd-login.c userland/base/ssh/sshd-child.c \
    userland/base/ssh/sshd-preauth.c userland/base/ssh/sshd-session.c userland/base/login/verify.c -lcrypto -lcrypt \
    -o "$work/sshd-pair" > "$work/build.log" 2>&1; then
	echo "build failed:"
	head -30 "$work/build.log"
	echo "sshd-pair-host: FAIL"
	exit 1
fi

# The client of the cases the host's ssh does not send, from the library's client parts.
if ! timeout 300 cc -std=gnu99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -fno-omit-frame-pointer -I. -Iuserland/base/ssh -Iplan/ws336/tests/host-crypto plan/ws336/tests/sshd-pair-client.c \
    $modules -lcrypto -o "$work/sshd-pair-client" > "$work/build-client.log" 2>&1; then
	echo "build of the client failed:"
	head -30 "$work/build-client.log"
	echo "sshd-pair-host: FAIL"
	exit 1
fi

# The keys (made by the host's ssh-keygen: they are inputs), the home and its authorized_keys, the shell.
for key in host_key client_key other_key1 other_key2 other_key3; do
	ssh-keygen -q -t ed25519 -N '' -C "$key" -f "$work/$key" || fail "ssh-keygen $key"
done
mkdir -m 700 "$work/home" "$work/home/.ssh"
cp "$work/client_key.pub" "$work/home/.ssh/authorized_keys"
chmod 600 "$work/home/.ssh/authorized_keys"
cat > "$work/shell.sh" <<'EOF'
#!/bin/sh
# The test account's login shell: a command runs under /bin/sh; a login prints that it ran, its terminal and TERM,
# and ends (argv[0] does not reach a script through #!, so the "-" of a login name is not seen here).
if [ "${1-}" = "-c" ]; then
	exec /bin/sh -c "$2"
fi
echo "login-shell $0"
tty
echo "TERM=${TERM-unset}"
exit 0
EOF
chmod 755 "$work/shell.sh"
head -c 3000000 /dev/urandom > "$work/big.bin"
head -c 20000 /dev/urandom > "$work/small.bin"

base_config() {
	cat <<EOF
HostKey $work/host_key
AuthorizedKeysFile .ssh/authorized_keys
PasswordAuthentication no
LoginGraceTime 60
Subsystem sftp /usr/lib/openssh/sftp-server
EOF
}

# Starts the driver for case $1 with the configuration in $work/$1.config; sets port.
start_driver() {
	(timeout 120 "$work/sshd-pair" "$work/$1.config" "$work/$1.port" "$work/$1.result" > "$work/$1.driver" 2>&1; \
	    echo "$?" > "$work/$1.driver-rc") &
	tries=0
	while [ ! -s "$work/$1.port" ] && [ "$tries" -lt 100 ]; do
		sleep 0.1
		tries=$((tries + 1))
	done
	port=$(cat "$work/$1.port" 2>/dev/null || echo 0)
}

# Waits for the driver of case $1 to end; sets driver_rc and result.
end_driver() {
	tries=0
	while [ ! -s "$work/$1.driver-rc" ] && [ "$tries" -lt 300 ]; do
		sleep 0.1
		tries=$((tries + 1))
	done
	driver_rc=$(cat "$work/$1.driver-rc" 2>/dev/null || echo none)
	result=$(cat "$work/$1.result" 2>/dev/null || echo none)
}

# Runs the host's ssh for case $1 with the rest as its arguments (options, the host 127.0.0.1, the command).
run_ssh() {
	name=$1
	shift
	timeout 90 env -i PATH="$PATH" TERM=xterm-test ssh -F /dev/null -o UserKnownHostsFile="$work/known_hosts" \
	    -o GlobalKnownHostsFile=/dev/null -o StrictHostKeyChecking=accept-new -o IdentitiesOnly=yes \
	    -o BatchMode=yes -o PasswordAuthentication=no -o KbdInteractiveAuthentication=no -o UpdateHostKeys=no \
	    -o ControlMaster=no -o ControlPath=none -p "$port" -l "$user" "$@"
}

# A command: its output, its error output and its exit status come back.
base_config > "$work/exec.config"
start_driver exec
run_ssh exec -o IdentityFile="$work/client_key" -o LogLevel=ERROR 127.0.0.1 'echo out; echo err >&2; exit 3' \
    > "$work/exec.out" 2> "$work/exec.err"
rc=$?
end_driver exec
[ "$rc" = 3 ] || fail "exec: exit status $rc"
[ "$(cat "$work/exec.out")" = out ] || fail "exec: stdout $(cat "$work/exec.out")"
grep -qx err "$work/exec.err" || fail "exec: stderr $(cat "$work/exec.err")"
[ "$result" = "status 0 granted 1" ] || fail "exec: monitor $result"
[ "$driver_rc" = 0 ] || fail "exec: driver $driver_rc"
grep -q 'Accepted login for' "$work/log" || fail "exec: no grant in the log"
echo "ok exec"

# The environment: LANG and LC_* taken, anything else not; sshd's own variables set.
base_config > "$work/env.config"
start_driver env
run_ssh env -o IdentityFile="$work/client_key" -o LogLevel=ERROR -o 'SetEnv=LANG=C.UTF-8 LC_TIME=C FOO=bar LD_PRELOAD=x' \
    127.0.0.1 'echo "L=$LANG T=$LC_TIME F=${FOO-unset} P=${LD_PRELOAD-unset} H=$HOME U=$USER S=$SHELL"; echo "C=$SSH_CONNECTION"; echo "K=$SSH_CLIENT"; echo "Y=${SSH_TTY-unset}"' \
    > "$work/env.out" 2> "$work/env.err"
rc=$?
end_driver env
[ "$rc" = 0 ] || fail "env: exit status $rc"
expected="L=C.UTF-8 T=C F=unset P=unset H=$work/home U=$user S=$work/shell.sh"
[ "$(sed -n 1p "$work/env.out")" = "$expected" ] || fail "env: $(sed -n 1p "$work/env.out")"
sed -n 2p "$work/env.out" | grep -Eq "^C=127\.0\.0\.1 [0-9]+ 127\.0\.0\.1 $port\$" || fail "env: $(sed -n 2p "$work/env.out")"
sed -n 3p "$work/env.out" | grep -Eq "^K=127\.0\.0\.1 [0-9]+ $port\$" || fail "env: $(sed -n 3p "$work/env.out")"
[ "$(sed -n 4p "$work/env.out")" = "Y=unset" ] || fail "env: $(sed -n 4p "$work/env.out")"
echo "ok env"

# Standard input: 3 MB through cat (the windows move both ways), with re-exchanges the client starts every 512 KiB,
# each signed by the monitor.
base_config > "$work/rekey.config"
start_driver rekey
run_ssh rekey -o IdentityFile="$work/client_key" -o LogLevel=DEBUG1 -o RekeyLimit=512K 127.0.0.1 'cat' \
    < "$work/big.bin" > "$work/rekey.out" 2> "$work/rekey.err"
rc=$?
end_driver rekey
[ "$rc" = 0 ] || fail "rekey: exit status $rc"
cmp -s "$work/big.bin" "$work/rekey.out" || fail "rekey: the data differs"
kex=$(grep -c 'SSH2_MSG_KEX_HYBRID_REPLY received\|SSH2_MSG_KEX_ECDH_REPLY received\|expecting SSH2_MSG_KEX' "$work/rekey.err")
[ "$kex" -ge 3 ] || fail "rekey: $kex key exchanges"
[ "$result" = "status 0 granted 1" ] || fail "rekey: monitor $result"
echo "ok rekey ($kex exchanges)"

# A terminal: the session opens it as the user, TERM from pty-req, the command runs on it; a login shell runs with
# no command.  The login record is refused on this host (no process list), which only the log shows.
base_config > "$work/pty.config"
start_driver pty
run_ssh pty -o IdentityFile="$work/client_key" -o LogLevel=ERROR -tt 127.0.0.1 'tty; echo "TERM=$TERM"; ls /proc/$$/fd | tr "\n" " "; echo' \
    > "$work/pty.out" 2> "$work/pty.err"
rc=$?
end_driver pty
[ "$rc" = 0 ] || fail "pty: exit status $rc"
tr -d '\r' < "$work/pty.out" > "$work/pty.txt"
grep -Eq '^/dev/pts/[0-9]+$' "$work/pty.txt" || fail "pty: $(cat "$work/pty.txt")"
grep -qx 'TERM=xterm-test' "$work/pty.txt" || fail "pty: TERM $(cat "$work/pty.txt")"
grep -qx '0 1 2 ' "$work/pty.txt" || fail "pty: descriptors $(cat "$work/pty.txt")"
grep -q 'no login record' "$work/log" || fail "pty: the record's refusal is not logged"
base_config > "$work/login.config"
start_driver login
run_ssh login -o IdentityFile="$work/client_key" -o LogLevel=ERROR -tt 127.0.0.1 > "$work/login.out" 2> "$work/login.err" < /dev/null
rc=$?
end_driver login
tr -d '\r' < "$work/login.out" > "$work/login.txt"
[ "$rc" = 0 ] || fail "login: exit status $rc"
grep -q '^login-shell ' "$work/login.txt" || fail "login: $(cat "$work/login.txt")"
grep -Eq '^/dev/pts/[0-9]+$' "$work/login.txt" || fail "login: no terminal $(cat "$work/login.txt")"
grep -qx 'TERM=xterm-test' "$work/login.txt" || fail "login: TERM $(cat "$work/login.txt")"
echo "ok pty"

# The descriptors of a command without a terminal: only its three streams (no monitor socket, no connection).
base_config > "$work/descriptors.config"
start_driver descriptors
run_ssh descriptors -o IdentityFile="$work/client_key" -o LogLevel=ERROR 127.0.0.1 'ls -l /proc/$$/fd | grep -c " -> "; ls /proc/$$/fd | tr "\n" " "' \
    > "$work/descriptors.out" 2> "$work/descriptors.err"
rc=$?
end_driver descriptors
[ "$rc" = 0 ] || fail "descriptors: exit status $rc"
[ "$(sed -n 2p "$work/descriptors.out")" = "0 1 2 " ] || fail "descriptors: $(cat "$work/descriptors.out")"
echo "ok descriptors"

# The end of a child by a signal is exit-signal.
base_config > "$work/signal.config"
start_driver signal
run_ssh signal -o IdentityFile="$work/client_key" -o LogLevel=DEBUG1 127.0.0.1 'kill -TERM $$' > "$work/signal.out" 2> "$work/signal.err"
rc=$?
end_driver signal
grep -q 'rtype exit-signal' "$work/signal.err" || fail "signal: no exit-signal"
[ "$rc" != 0 ] || fail "signal: exit status 0"
echo "ok signal"

# The sftp subsystem: a file up and back.
base_config > "$work/sftp.config"
start_driver sftp
printf 'put %s up.bin\nget up.bin %s\n' "$work/small.bin" "$work/down.bin" > "$work/sftp.batch"
timeout 90 env -i PATH="$PATH" sftp -F /dev/null -o UserKnownHostsFile="$work/known_hosts" -o GlobalKnownHostsFile=/dev/null \
    -o StrictHostKeyChecking=accept-new -o IdentitiesOnly=yes -o IdentityFile="$work/client_key" -o BatchMode=yes \
    -o LogLevel=ERROR -b "$work/sftp.batch" -P "$port" "$user@127.0.0.1" > "$work/sftp.out" 2> "$work/sftp.err"
rc=$?
end_driver sftp
[ "$rc" = 0 ] || fail "sftp: exit status $rc $(cat "$work/sftp.err")"
cmp -s "$work/small.bin" "$work/down.bin" || fail "sftp: the file differs"
cmp -s "$work/small.bin" "$work/home/up.bin" || fail "sftp: the uploaded file differs"
echo "ok sftp"

# A key that is not authorized: refused, no session.
base_config > "$work/refused.config"
start_driver refused
run_ssh refused -o IdentityFile="$work/other_key1" -o LogLevel=ERROR 127.0.0.1 true > "$work/refused.out" 2> "$work/refused.err"
rc=$?
end_driver refused
[ "$rc" = 255 ] || fail "refused: exit status $rc"
grep -q 'Permission denied' "$work/refused.err" || fail "refused: $(cat "$work/refused.err")"
[ "$result" = "status 1 granted 0" ] || fail "refused: monitor $result"
echo "ok refused"

# MaxAuthTries 2 and three keys that are not authorized: the third ends the connection with reason 14.
{ base_config; echo "MaxAuthTries 2"; } > "$work/tries.config"
start_driver tries
run_ssh tries -o IdentityFile="$work/other_key1" -o IdentityFile="$work/other_key2" -o IdentityFile="$work/other_key3" \
    -o LogLevel=ERROR 127.0.0.1 true > "$work/tries.out" 2> "$work/tries.err"
rc=$?
end_driver tries
[ "$rc" = 255 ] || fail "tries: exit status $rc"
grep -q 'Too many authentication failures' "$work/tries.err" || fail "tries: $(cat "$work/tries.err")"
grep -q 'too many authentication failures' "$work/log" || fail "tries: not in the log"
grep -q 'refused request' "$work/log" && fail "tries: the monitor saw a request over the limit"
echo "ok tries"

# The cases of the library's client: requests on a closing channel, the order of the channel's end, a change of user
# or service, refused and repeated requests, a signal.
for case in closing user-change service requests signal; do
	base_config > "$work/raw-$case.config"
	start_driver "raw-$case"
	timeout 90 "$work/sshd-pair-client" "$port" "$user" "$work/client_key" "$case" > "$work/raw-$case.out" 2>&1
	rc=$?
	end_driver "raw-$case"
	[ "$rc" = 0 ] || fail "raw $case: $(cat "$work/raw-$case.out")"
	grep -qx "ok $case" "$work/raw-$case.out" || fail "raw $case: $(cat "$work/raw-$case.out")"
done
grep -q 'refused request' "$work/log" && fail "raw: the monitor saw a request it refuses"
echo "ok raw cases"

# Nothing the sanitizers found in any process (the children write their reports to the driver's standard error).
if grep -l 'ERROR: AddressSanitizer\|runtime error:' "$work"/*.driver > /dev/null 2>&1; then
	fail "sanitizer: $(grep -l 'ERROR: AddressSanitizer\|runtime error:' "$work"/*.driver)"
fi

sync
if [ "$status" = 0 ]; then echo "sshd-pair-host: PASS"; else echo "sshd-pair-host: FAIL"; fi
exit "$status"
