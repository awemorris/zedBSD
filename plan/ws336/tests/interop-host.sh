#!/bin/sh
# The host interoperation test of zedBSD's ssh (userland/base/ssh): the programs are built for the host with ASan and
# UBSan (build-host.sh, the host's OpenSSL 3.5 through the shims) and run against the host's own OpenSSH.
#
# Peer A: the host's OpenSSH sshd, started as this user (never root) from a temporary directory with its own
# configuration (HostKey and AuthorizedKeysFile there, PidFile none, UsePAM no, StrictModes no, ListenAddress
# 127.0.0.1, free ports), one ssh-ed25519 host key and the one key exchange, host key type and cipher these programs
# speak.  zedBSD's ssh runs with HOME in the temporary directory and -F a temporary configuration, so the user's own
# ~/.ssh, the system's sshd and /etc/ssh are never read or written.  Every command has a time limit; the servers and
# everything they started are stopped at the end.
#
# Checks: exec and the exit status (3 gives 3, a signal gives 255), 8 MiB through stdin and back (stdout and stderr,
# cmp), -tt with the terminal size (stty size) and a size change, the "~." escape, known_hosts (accept-new writes the
# "[host]:port" line, a known key passes with StrictHostKeyChecking yes, a changed key and an unknown host in batch mode
# are refused, the question on the terminal), the HostName of ssh_config as the name looked up and written (and
# OpenSSH's ssh accepts that file), 64 MiB through a server that re-keys every 1 MiB (with the re-exchanges seen),
# ServerAliveInterval (answered keepalives, and a server that stops answering), a server with only curve25519-sha256
# ("no matching", 255), ConnectTimeout, refused keys and passwords asked on the terminal, -N, -n, a shell fed by
# stdin.  Expected values come from the formats (sshd(8) known_hosts lines) and from the test's own data; OpenSSH is
# the peer, never the source of an expected value.
#   sh plan/ws336/tests/interop-host.sh
# SSH_SOURCE_DIR=<dir> builds from another source directory (a changed copy under build/tmp).
# Last line: interop-host: PASS or interop-host: FAIL.  Output under build/tmp/interop-host.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=${INTEROP_OUT:-build/tmp/interop-host}
fresh_out "$out"
out=$(cd "$out" && pwd -P)
python=/usr/bin/python3
sshd_program=/usr/sbin/sshd
openssh_keygen=/usr/bin/ssh-keygen
openssh_client=/usr/bin/ssh

checks=0
failures=0
fail() {
	failures=$((failures + 1))
	echo "FAIL: $*"
}
pass() {
	checks=$((checks + 1))
}

# check DESCRIPTION CONDITION-STATUS: counts a check that passed when the status is 0.
check() {
	checks=$((checks + 1))
	if [ "$2" != 0 ]; then
		fail "$1"
	fi
}

# expect_eq DESCRIPTION ACTUAL EXPECTED
expect_eq() {
	checks=$((checks + 1))
	if [ "$2" != "$3" ]; then
		fail "$1: got [$2] expected [$3]"
	fi
}

for tool in "$sshd_program" "$openssh_keygen" "$openssh_client" "$python"; do
	if [ ! -x "$tool" ]; then
		echo "interop-host: FAIL ($tool is not installed)"
		exit 1
	fi
done

if ! SSH_SOURCE_DIR=${SSH_SOURCE_DIR:-userland/base/ssh} sh plan/ws336/tests/build-host.sh "$out/bin" ssh ssh-keygen \
    > "$out/build.log" 2>&1; then
	tail -20 "$out/build.log"
	echo "interop-host: FAIL (build)"
	exit 1
fi
ssh="$out/bin/ssh"
keygen="$out/bin/ssh-keygen"

# 99 is the exit value of a sanitizer report, never an answer of the program.
ASAN_OPTIONS=detect_leaks=1:exitcode=99
UBSAN_OPTIONS=halt_on_error=1:exitcode=99
export ASAN_OPTIONS UBSAN_OPTIONS

work="$out/work"
mkdir -p "$work/home" "$work/server"
chmod 700 "$work/home"
HOME="$work/home"
export HOME
user=$(id -un)

# The helpers in python: a free port, waiting for a port, a silent listener, and a program run on a terminal.
cat > "$out/helper.py" <<'EOF'
import os, pty, select, signal, socket, struct, sys, termios, fcntl, time

def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    print(port)

def wait_port(port, seconds):
    end = time.time() + seconds
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 1).close()
            return 0
        except OSError:
            time.sleep(0.1)
    return 1

def silent(port, seconds):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(4)
    held = []
    end = time.time() + seconds
    s.settimeout(0.5)
    while time.time() < end:
        try:
            held.append(s.accept()[0])
        except OSError:
            pass

def set_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

# run_pty OUT ROWS COLS SECONDS [ON PATTERN ACTION ARG]... -- COMMAND...
# ACTION: write TEXT (\r and \n are taken), resize ROWS:COLS.  Each rule fires once, in order.
def run_pty(args):
    out, rows, cols, seconds = args[0], int(args[1]), int(args[2]), float(args[3])
    rules = []
    i = 4
    while args[i] != "--":
        rules.append((args[i + 1].encode(), args[i + 2], args[i + 3]))
        i += 4
    command = args[i + 1:]
    pid, master = pty.fork()
    if pid == 0:
        set_size(0, rows, cols)
        os.execv(command[0], command)
    data = b""
    seen = 0
    end = time.time() + seconds
    status = None
    while True:
        left = end - time.time()
        if left <= 0:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            status = 124
            break
        ready, _, _ = select.select([master], [], [], min(left, 0.2))
        if ready:
            try:
                chunk = os.read(master, 65536)
            except OSError:
                chunk = b""
            if not chunk:
                break
            data += chunk
        while rules and rules[0][0] in data[seen:]:
            pattern, action, arg = rules.pop(0)
            seen = data.index(pattern, seen) + len(pattern)
            if action == "write":
                time.sleep(0.2)
                os.write(master, arg.replace("\\r", "\r").replace("\\n", "\n").encode())
            elif action == "resize":
                r, c = arg.split(":")
                set_size(master, int(r), int(c))
    if status is None:
        _, raw = os.waitpid(pid, 0)
        if os.WIFEXITED(raw):
            status = os.WEXITSTATUS(raw)
        else:
            status = 128 + os.WTERMSIG(raw)
    open(out, "wb").write(data)
    return status

what = sys.argv[1]
if what == "free-port":
    free_port()
elif what == "wait-port":
    sys.exit(wait_port(int(sys.argv[2]), float(sys.argv[3])))
elif what == "silent":
    silent(int(sys.argv[2]), float(sys.argv[3]))
elif what == "pty":
    sys.exit(run_pty(sys.argv[2:]))
EOF
helper() {
	"$python" -I "$out/helper.py" "$@"
}

# The processes to stop at the end: each sshd and every process under it (a stopped one is continued first).
server_pids=""
stop_servers() {
	for pid in $server_pids; do
		"$python" -I - "$pid" <<'EOF'
import os, signal, sys
root = int(sys.argv[1])
children = {}
for entry in os.listdir("/proc"):
    if not entry.isdigit():
        continue
    try:
        fields = open("/proc/%s/stat" % entry).read().rsplit(")", 1)[1].split()
    except OSError:
        continue
    children.setdefault(int(fields[1]), []).append(int(entry))
tree = []
pending = [root]
while pending:
    pid = pending.pop()
    tree.append(pid)
    pending.extend(children.get(pid, []))
for pid in tree:
    for number in (signal.SIGCONT, signal.SIGKILL):
        try:
            os.kill(pid, number)
        except OSError:
            pass
EOF
	done
	server_pids=""
}
trap stop_servers EXIT

# The keys: the server's host key and a second key to stand for a changed one (OpenSSH's ssh-keygen, the server's
# side), the user's key and a key the server does not know (zedBSD's ssh-keygen).
timeout 30 "$openssh_keygen" -q -t ed25519 -N '' -C host -f "$work/server/host_key" > /dev/null 2>&1
timeout 30 "$openssh_keygen" -q -t ed25519 -N '' -C other -f "$work/server/other_key" > /dev/null 2>&1
mkdir -p "$HOME/.ssh"
chmod 700 "$HOME/.ssh"
timeout 30 "$keygen" -q -t ed25519 -N '' -C user -f "$HOME/.ssh/id_ed25519" > /dev/null 2>&1
check "zedBSD's ssh-keygen made the user key" $?
timeout 30 "$keygen" -q -t ed25519 -N '' -C stranger -f "$work/stranger_key" > /dev/null 2>&1
check "zedBSD's ssh-keygen made the second user key" $?
cp "$HOME/.ssh/id_ed25519.pub" "$work/server/authorized_keys"
host_blob=$(cut -d ' ' -f 2 "$work/server/host_key.pub")
other_blob=$(cut -d ' ' -f 2 "$work/server/other_key.pub")

# start_sshd NAME PORT EXTRA-LINES: starts the host's sshd as this user with a configuration of its own.
start_sshd() {
	name=$1
	port=$2
	extra=$3
	{
		echo "Port $port"
		echo "ListenAddress 127.0.0.1"
		echo "HostKey $work/server/host_key"
		echo "AuthorizedKeysFile $work/server/authorized_keys"
		echo "PidFile none"
		echo "UsePAM no"
		echo "StrictModes no"
		echo "HostKeyAlgorithms ssh-ed25519"
		echo "Ciphers chacha20-poly1305@openssh.com"
		echo "$extra"
	} > "$work/server/$name.conf"
	timeout 600 "$sshd_program" -D -e -f "$work/server/$name.conf" > "$work/server/$name.log" 2>&1 &
	server_pids="$server_pids $!"
	if ! helper wait-port "$port" 10; then
		fail "sshd $name did not start (log $work/server/$name.log)"
		return 1
	fi
	return 0
}

port_a=$(helper free-port)
port_rekey=$(helper free-port)
port_old=$(helper free-port)
port_password=$(helper free-port)
port_silent=$(helper free-port)
start_sshd a "$port_a" "KexAlgorithms mlkem768x25519-sha256
PasswordAuthentication no" || { echo "interop-host: FAIL"; exit 1; }
start_sshd rekey "$port_rekey" "KexAlgorithms mlkem768x25519-sha256
RekeyLimit 1M
PasswordAuthentication no" || { echo "interop-host: FAIL"; exit 1; }
start_sshd old "$port_old" "KexAlgorithms curve25519-sha256" || { echo "interop-host: FAIL"; exit 1; }
start_sshd password "$port_password" "KexAlgorithms mlkem768x25519-sha256
PasswordAuthentication yes
PubkeyAuthentication no" || { echo "interop-host: FAIL"; exit 1; }
timeout 120 "$python" -I "$out/helper.py" silent "$port_silent" 110 &
server_pids="$server_pids $!"
helper wait-port "$port_silent" 10 || fail "the silent listener did not start"

config="$work/ssh_config"
cat > "$config" <<EOF
Host alias-a
    HostName 127.0.0.1
    Port $port_a
Host *
    GlobalKnownHostsFile /dev/null
    BatchMode yes
EOF
known="$HOME/.ssh/known_hosts"

# zssh ARGS...: zedBSD's ssh with the test's configuration and a time limit.
zssh() {
	timeout 60 "$ssh" -F "$config" "$@"
}

# --- known_hosts: accept-new adds the "[host]:port" line (sshd(8) SSH_KNOWN_HOSTS FILE FORMAT) and connects.
zssh -o StrictHostKeyChecking=accept-new -p "$port_a" 127.0.0.1 true > "$work/t1.out" 2> "$work/t1.err"
expect_eq "accept-new connection exit" "$?" 0
expect_eq "accept-new wrote one line" "$(cat "$known" 2>/dev/null)" "[127.0.0.1]:$port_a ssh-ed25519 $host_blob"

zssh -o StrictHostKeyChecking=yes -p "$port_a" 127.0.0.1 'echo known' > "$work/t2.out" 2> "$work/t2.err"
expect_eq "known host with StrictHostKeyChecking yes exit" "$?" 0
expect_eq "known host output" "$(cat "$work/t2.out")" known
expect_eq "a known host adds no line" "$(wc -l < "$known" | tr -d ' ')" 1

# --- exec and exit status.
zssh -p "$port_a" 127.0.0.1 'echo hello; exit 3' > "$work/t3.out" 2> "$work/t3.err"
expect_eq "exit 3 gives 3" "$?" 3
expect_eq "exec output" "$(cat "$work/t3.out")" hello
zssh -p "$port_a" 127.0.0.1 'exit 0' > /dev/null 2>&1
expect_eq "exit 0 gives 0" "$?" 0
zssh -p "$port_a" 127.0.0.1 'exit 255' > /dev/null 2>&1
expect_eq "exit 255 gives 255" "$?" 255
zssh -p "$port_a" 127.0.0.1 'kill -TERM $$' > /dev/null 2> "$work/t4.err"
expect_eq "a command killed by a signal gives 255" "$?" 255
zssh -p "$port_a" 127.0.0.1 echo one two '  three' > "$work/t5.out" 2>&1
expect_eq "the command arguments are joined with spaces" "$(cat "$work/t5.out")" "one two three"

# --- 8 MiB through stdin and back, on stdout and on stderr.
head -c 8388608 /dev/urandom > "$work/data8"
zssh -p "$port_a" 127.0.0.1 cat < "$work/data8" > "$work/data8.out" 2> "$work/t6.err"
expect_eq "8 MiB round trip exit" "$?" 0
cmp -s "$work/data8" "$work/data8.out"
check "8 MiB round trip on stdout is the same bytes" $?
zssh -p "$port_a" 127.0.0.1 'cat 1>&2' < "$work/data8" > "$work/t7.out" 2> "$work/data8.err"
expect_eq "8 MiB round trip on stderr exit" "$?" 0
cmp -s "$work/data8" "$work/data8.err"
check "8 MiB round trip on stderr is the same bytes" $?
zssh -p "$port_a" 127.0.0.1 cat < "$work/data8" | cmp -s - "$work/data8"
check "8 MiB round trip into a pipe is the same bytes" $?

# --- -n, a shell fed by stdin, -N.
zssh -n -p "$port_a" 127.0.0.1 cat > "$work/t8.out" 2>&1 < "$work/data8"
expect_eq "-n gives the command an empty input" "$?:$(wc -c < "$work/t8.out" | tr -d ' ')" "0:0"
echo 'echo shell-ran; exit 4' | zssh -p "$port_a" 127.0.0.1 > "$work/t9.out" 2> "$work/t9.err"
expect_eq "a shell fed by stdin exit" "$?" 4
expect_eq "a shell fed by stdin output" "$(cat "$work/t9.out")" shell-ran
timeout 60 "$ssh" -F "$config" -N -p "$port_a" 127.0.0.1 > /dev/null 2> "$work/t10.err" &
n_pid=$!
sleep 2
kill -TERM "$n_pid" 2>/dev/null
wait "$n_pid"
expect_eq "-N runs until a signal and gives 255" "$?" 255

# --- a terminal: -tt with the size, a size change, and the "~." escape.
helper pty "$work/pty1.out" 37 123 30 on SIZE1 resize 40:100 -- "$ssh" -F "$config" -tt -p "$port_a" 127.0.0.1 \
    'printf "SIZE1 %s\n" "$(stty size)"; sleep 2; printf "SIZE2 %s\n" "$(stty size)"'
expect_eq "-tt exit" "$?" 0
grep -q 'SIZE1 37 123' "$work/pty1.out"
check "-tt gives the remote terminal the local size" $?
grep -q 'SIZE2 40 100' "$work/pty1.out"
check "a size change reaches the remote terminal" $?
start=$(date +%s)
helper pty "$work/pty2.out" 24 80 30 on READY write '\r~.' -- "$ssh" -F "$config" -tt -p "$port_a" 127.0.0.1 \
    'echo READY; sleep 25'
expect_eq "the ~. escape ends the connection with 255" "$?" 255
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -lt 15 ]
check "the ~. escape ends it at once (took ${elapsed} s)" $?
helper pty "$work/pty3.out" 24 80 30 on READY write '~~tilde a~b\r' -- "$ssh" -F "$config" -tt -p "$port_a" \
    127.0.0.1 'echo READY; read line; echo "GOT[$line]"'
expect_eq "~~ on a terminal exit" "$?" 0
grep -q 'GOT\[~tilde a~b\]' "$work/pty3.out"
check "~~ at the start of a line sends one ~, and a ~ inside a line is sent as it is" $?

# --- known_hosts refusals and the question.
printf '[127.0.0.1]:%s ssh-ed25519 %s\n' "$port_a" "$other_blob" > "$work/known_changed"
zssh -o UserKnownHostsFile="$work/known_changed" -p "$port_a" 127.0.0.1 'echo reached' > "$work/t11.out" \
    2> "$work/t11.err"
expect_eq "a changed host key is refused with 255" "$?:$(cat "$work/t11.out")" "255:"
zssh -o UserKnownHostsFile="$work/known_changed" -o StrictHostKeyChecking=no -p "$port_a" 127.0.0.1 'echo reached' \
    > "$work/t12.out" 2> "$work/t12.err"
expect_eq "a changed host key is refused also with StrictHostKeyChecking no" "$?:$(cat "$work/t12.out")" "255:"
expect_eq "a refused key adds no line" "$(cat "$work/known_changed")" "[127.0.0.1]:$port_a ssh-ed25519 $other_blob"
zssh -o UserKnownHostsFile="$work/known_absent" -p "$port_a" 127.0.0.1 'echo reached' > "$work/t13.out" \
    2> "$work/t13.err"
expect_eq "an unknown host in batch mode (ask) is refused with 255" "$?:$(cat "$work/t13.out")" "255:"
zssh -o UserKnownHostsFile="$work/known_absent" -o StrictHostKeyChecking=yes -o BatchMode=no -p "$port_a" 127.0.0.1 \
    'echo reached' > "$work/t14.out" 2> "$work/t14.err"
expect_eq "an unknown host with StrictHostKeyChecking yes is refused with 255" "$?:$(cat "$work/t14.out")" "255:"
[ ! -e "$work/known_absent" ]
check "a refused unknown host writes no file" $?
helper pty "$work/pty4.out" 24 80 30 on '(yes/no' write 'yes\r' -- "$ssh" -F "$config" -o BatchMode=no \
    -o UserKnownHostsFile="$work/known_asked" -p "$port_a" 127.0.0.1 'echo ASKED-OK'
expect_eq "answering yes to the question exit" "$?" 0
grep -q ASKED-OK "$work/pty4.out"
check "answering yes to the question connects" $?
expect_eq "answering yes adds the line" "$(cat "$work/known_asked" 2>/dev/null)" \
    "[127.0.0.1]:$port_a ssh-ed25519 $host_blob"
helper pty "$work/pty5.out" 24 80 30 on '(yes/no' write 'no\r' -- "$ssh" -F "$config" -o BatchMode=no \
    -o UserKnownHostsFile="$work/known_refused" -p "$port_a" 127.0.0.1 'echo ASKED-OK'
expect_eq "answering no to the question gives 255" "$?" 255
[ ! -e "$work/known_refused" ]
check "answering no writes no file" $?

# --- HostName: the name looked up and written is the HostName, and OpenSSH's ssh accepts the file.
zssh -o UserKnownHostsFile="$work/known_alias" -o StrictHostKeyChecking=accept-new alias-a 'echo alias-ok' \
    > "$work/t15.out" 2> "$work/t15.err"
expect_eq "connection through a Host block with HostName exit" "$?" 0
expect_eq "connection through a Host block with HostName output" "$(cat "$work/t15.out")" alias-ok
expect_eq "the line names the HostName and the port" "$(cat "$work/known_alias" 2>/dev/null)" \
    "[127.0.0.1]:$port_a ssh-ed25519 $host_blob"
timeout 60 "$openssh_client" -F /dev/null -o BatchMode=yes -o StrictHostKeyChecking=yes -o IdentitiesOnly=yes \
    -o UserKnownHostsFile="$work/known_alias" -o GlobalKnownHostsFile=/dev/null -i "$HOME/.ssh/id_ed25519" \
    -p "$port_a" 127.0.0.1 'echo openssh-ok' > "$work/t16.out" 2> "$work/t16.err"
expect_eq "OpenSSH's ssh accepts the known_hosts line zedBSD's ssh wrote" "$?:$(cat "$work/t16.out")" "0:openssh-ok"
zssh -o UserKnownHostsFile="$work/known_alias" -o StrictHostKeyChecking=yes -p "$port_a" 127.0.0.1 'echo again' \
    > "$work/t17.out" 2> "$work/t17.err"
expect_eq "the same file serves the address and port directly" "$?:$(cat "$work/t17.out")" "0:again"

# --- re-keying: 64 MiB through a server that re-keys every 1 MiB, with the re-exchanges seen.
head -c 67108864 /dev/urandom > "$work/data64"
zssh -o StrictHostKeyChecking=accept-new -vv -p "$port_rekey" 127.0.0.1 cat < "$work/data64" 2> "$work/t18.err" \
    | cmp -s - "$work/data64"
check "64 MiB round trip through a re-keying server is the same bytes" $?
rekeys=$(grep -c 'host key checked again is the same' "$work/t18.err")
[ "$rekeys" -ge 60 ]
check "the server re-keyed at least 60 times and the same host key was shown each time ($rekeys)" $?

# --- ServerAliveInterval: answered keepalives, and a server that stops answering.
zssh -o ServerAliveInterval=1 -p "$port_a" 127.0.0.1 'sleep 3; echo alive-ok' > "$work/t19.out" 2> "$work/t19.err"
expect_eq "keepalives the server answers keep the session" "$?:$(cat "$work/t19.out")" "0:alive-ok"
start=$(date +%s)
zssh -o ServerAliveInterval=1 -o ServerAliveCountMax=2 -p "$port_a" 127.0.0.1 'kill -STOP $PPID; sleep 50' \
    > "$work/t20.out" 2> "$work/t20.err"
expect_eq "a server that stops answering keepalives gives 255" "$?" 255
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -lt 20 ]
check "the unanswered keepalives end it in a few seconds (took ${elapsed} s)" $?
grep -q 'not responding' "$work/t20.err"
check "the timeout is reported" $?

# --- a server without the key exchange: "no matching", 255.
zssh -o StrictHostKeyChecking=accept-new -p "$port_old" 127.0.0.1 'echo reached' > "$work/t21.out" 2> "$work/t21.err"
expect_eq "a server with only curve25519-sha256 gives 255" "$?:$(cat "$work/t21.out")" "255:"
grep -q 'no matching key exchange method' "$work/t21.err"
check "the failed key exchange says no matching key exchange method" $?

# --- ConnectTimeout against a server that never sends its identification.
start=$(date +%s)
zssh -o ConnectTimeout=2 -p "$port_silent" 127.0.0.1 true > /dev/null 2> "$work/t22.err"
expect_eq "ConnectTimeout gives 255" "$?" 255
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -lt 10 ]
check "ConnectTimeout ends the wait in time (took ${elapsed} s)" $?

# --- refused authentication: a key the server does not know, an unknown user, passwords on the terminal.
chmod 600 "$work/stranger_key"
zssh -i "$work/stranger_key" -p "$port_a" 127.0.0.1 'echo reached' > "$work/t23.out" 2> "$work/t23.err"
expect_eq "a key the server does not know gives 255" "$?:$(cat "$work/t23.out")" "255:"
grep -q 'Permission denied (publickey' "$work/t23.err"
check "the refusal names the methods left" $?
zssh -p "$port_a" "nobody-$user@127.0.0.1" 'echo reached' > "$work/t24.out" 2> "$work/t24.err"
expect_eq "an unknown user gives 255" "$?:$(cat "$work/t24.out")" "255:"
zssh -i "$work/missing_key" -p "$port_a" 127.0.0.1 'echo reached' > "$work/t25.out" 2> "$work/t25.err"
expect_eq "a missing identity file named with -i gives 255" "$?" 255
grep -q 'not accessible' "$work/t25.err"
check "a missing identity file named with -i is reported" $?
cp "$HOME/.ssh/id_ed25519" "$work/open_key"
chmod 644 "$work/open_key"
zssh -i "$work/open_key" -p "$port_a" 127.0.0.1 'echo reached' > "$work/t26.out" 2> "$work/t26.err"
expect_eq "a key file others may read is not used" "$?:$(cat "$work/t26.out")" "255:"
zssh -o StrictHostKeyChecking=accept-new -p "$port_password" 127.0.0.1 'echo reached' > "$work/t27.out" \
    2> "$work/t27.err"
expect_eq "batch mode asks no password and gives 255" "$?:$(cat "$work/t27.out")" "255:"
helper pty "$work/pty6.out" 24 80 40 on "password:" write 'wrong-1\r' on "password:" write 'wrong-2\r' \
    on "password:" write 'wrong-3\r' -- "$ssh" -F "$config" -o BatchMode=no -p "$port_password" 127.0.0.1 'echo reached'
expect_eq "three wrong passwords give 255" "$?" 255
prompts=$(grep -c "$user@127.0.0.1's password:" "$work/pty6.out")
expect_eq "the password is asked three times" "$prompts" 3
grep -q 'wrong-' "$work/pty6.out"
expect_eq "the password is not echoed" "$?" 1

# --- the sshd processes are still serving after all of the above.
zssh -p "$port_a" 127.0.0.1 'echo still-serving' > "$work/t28.out" 2> "$work/t28.err"
expect_eq "the server still serves" "$?:$(cat "$work/t28.out")" "0:still-serving"

stop_servers
sync
pass
echo "interop-host: $checks checks, $failures failed"
if [ "$failures" = 0 ]; then
	echo "interop-host: PASS"
	exit 0
fi
echo "interop-host: FAIL"
exit 1
