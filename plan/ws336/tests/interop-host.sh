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
#
# Peer B: zedBSD's sshd (the real listener, monitor and children, without root), built with the test's shim
# (sshd-host-shim.c: the account the test runs as gets the home and the shell of the server's own directory, the log
# goes to a file there), listening on 127.0.0.1 and checked to be the test's own process before any client talks to
# it.  From the host's OpenSSH ssh, scp and sftp: exec and the exit status, 8 MiB through stdin and back, -tt with
# the terminal size, three sessions over one ControlMaster connection, scp 8 MiB both ways, sftp -b ls, put, get,
# mkdir and rename, no post-quantum warning, KexAlgorithms curve25519-sha256 and Ciphers aes128-ctr refused with the
# server still serving, -X and -A (x11-req and auth-agent-req@openssh.com refused, the session goes on), and a shell
# that cannot be executed (exec refused).  From zedBSD's ssh: exec, 8 MiB, -tt, the exec refusal.  MaxStartups (ten
# connections that send nothing, the eleventh closed, a login frees a place) and sshd -t.
# SSH_SOURCE_DIR=<dir> builds from another source directory (a changed copy under build/tmp).
# Last line: interop-host: PASS or interop-host: FAIL.  Output under build/tmp/interop-host.
# SSH_CRYPTO_INCLUDE=<dir whose compat/crypto/ holds libcrypto-compat's headers> and SSH_CRYPTO_LIB=<what to link for
# it> build against libcrypto-compat instead of the host's OpenSSL (plan/ws336/tests/compat-host.sh sets both).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
crypto_include=${SSH_CRYPTO_INCLUDE:-plan/ws336/tests/host-crypto}
crypto_lib=${SSH_CRYPTO_LIB:--lcrypto}
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

if ! SSH_SOURCE_DIR=${SSH_SOURCE_DIR:-userland/base/ssh} sh plan/ws336/tests/build-host.sh "$out/bin" ssh ssh-keygen sftp-server \
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

def holder(port, root):
    inodes = set()
    for line in open("/proc/net/tcp").read().splitlines()[1:]:
        fields = line.split()
        if fields[3] == "0A" and fields[1] == "0100007F:%04X" % port:
            inodes.add("socket:[%s]" % fields[9])
    children = {}
    for entry in os.listdir("/proc"):
        if entry.isdigit():
            try:
                parent = int(open("/proc/%s/stat" % entry).read().rsplit(")", 1)[1].split()[1])
            except OSError:
                continue
            children.setdefault(parent, []).append(int(entry))
    pending = [root]
    while pending:
        pid = pending.pop(0)
        pending.extend(children.get(pid, []))
        try:
            links = [os.readlink("/proc/%d/fd/%s" % (pid, fd)) for fd in os.listdir("/proc/%d/fd" % pid)]
        except OSError:
            continue
        if inodes & set(links):
            print(pid)
            return 0
    return 1

def owner(port, root):
    inodes = set()
    for line in open("/proc/net/tcp").read().splitlines()[1:]:
        fields = line.split()
        local, state, inode = fields[1], fields[3], fields[9]
        if state == "0A" and local == "0100007F:%04X" % port:
            inodes.add("socket:[%s]" % inode)
    if not inodes:
        return 1
    children = {}
    for entry in os.listdir("/proc"):
        if entry.isdigit():
            try:
                parent = int(open("/proc/%s/stat" % entry).read().rsplit(")", 1)[1].split()[1])
            except OSError:
                continue
            children.setdefault(parent, []).append(int(entry))
    pending = [root]
    while pending:
        pid = pending.pop()
        pending.extend(children.get(pid, []))
        try:
            links = [os.readlink("/proc/%d/fd/%s" % (pid, fd)) for fd in os.listdir("/proc/%d/fd" % pid)]
        except OSError:
            continue
        if inodes & set(links):
            return 0
    return 1

def closer(port, seconds):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(4)
    end = time.time() + seconds
    s.settimeout(0.5)
    while time.time() < end:
        try:
            c = s.accept()[0]
            c.close()
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

# startups PORT LIMIT LOG -- LOGIN-COMMAND...: MaxStartups.  LIMIT connections that send nothing each get the
# server's identification line; one more is closed at once.  One of them is let go, the login command (a session
# that stays a while) logs in (seen as one more "Accepted login" line in the server's LOG), and then one more
# connection is taken and the next closed again.
def ident(port):
    s = socket.create_connection(("127.0.0.1", port), 5)
    s.settimeout(5)
    data = b""
    try:
        while b"\n" not in data:
            chunk = s.recv(256)
            if not chunk:
                return s, False
            data += chunk
    except OSError:
        return s, False
    return s, data.startswith(b"SSH-2.0-")

def startups(args):
    import subprocess
    port, limit, log = int(args[0]), int(args[1]), args[2]
    command = args[4:]
    def accepted():
        try:
            return open(log, errors="replace").read().count("Accepted login")
        except OSError:
            return 0
    held = []
    for i in range(limit):
        s, ok = ident(port)
        held.append(s)
        if not ok:
            print("connection %d got no identification" % (i + 1))
            return 1
    s, ok = ident(port)
    s.close()
    if ok:
        print("connection %d was not closed" % (limit + 1))
        return 1
    held.pop().close()
    time.sleep(1)
    before = accepted()
    login = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + 30
    while accepted() == before and time.time() < end:
        time.sleep(0.2)
    if accepted() == before:
        print("the login was not granted")
        return 1
    time.sleep(1)
    s, ok = ident(port)
    held.append(s)
    if not ok:
        print("after a login, a connection got no identification")
        return 1
    s, ok = ident(port)
    s.close()
    if ok:
        print("after a login and one more, a connection was not closed")
        return 1
    status = login.wait(60)
    for s in held:
        s.close()
    if status != 0:
        print("the login ended with %d" % status)
        return 1
    print("ok")
    return 0

# fds PID: the number of descriptors the process has open.
def fds(pid):
    print(len(os.listdir("/proc/%d/fd" % pid)))
    return 0

what = sys.argv[1]
if what == "free-port":
    free_port()
elif what == "wait-port":
    sys.exit(wait_port(int(sys.argv[2]), float(sys.argv[3])))
elif what == "silent":
    silent(int(sys.argv[2]), float(sys.argv[3]))
elif what == "owner":
    sys.exit(owner(int(sys.argv[2]), int(sys.argv[3])))
elif what == "closer":
    closer(int(sys.argv[2]), float(sys.argv[3]))
elif what == "pty":
    sys.exit(run_pty(sys.argv[2:]))
elif what == "holder":
    sys.exit(holder(int(sys.argv[2]), int(sys.argv[3])))
elif what == "fds":
    sys.exit(fds(int(sys.argv[2])))
elif what == "startups":
    sys.exit(startups(sys.argv[2:]))
EOF
helper() {
	"$python" -I "$out/helper.py" "$@"
}

# The processes to stop at the end: each sshd and every process under it (a stopped one is continued first).
server_pids=""
owned=""
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
trap 'stop_servers; exit 1' INT TERM HUP

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

# start_sshd NAME PORT EXTRA-LINES: starts the host's sshd as this user with a configuration of its own, and fails
# closed unless the listening socket of the port belongs to that sshd.  The sshd runs as this account, so every
# command runs through the account's login shell with "-c" in its home directory: ForceCommand runs the client's
# command with eval (or the instance's own command), so a session without a command (a shell request) never starts an
# interactive login shell, which would write the account's history; PermitUserRC no keeps ~/.ssh/rc from running, and
# AuthorizedKeysFile is the test's own, so the account's ~/.ssh is not read.
start_sshd() {
	name=$1
	port=$2
	extra=$3
	{
		echo "$extra"
		echo "ForceCommand eval \"\$SSH_ORIGINAL_COMMAND\""
		echo "PermitUserRC no"
		echo "PermitUserEnvironment no"
		echo "X11Forwarding no"
		echo "AllowAgentForwarding no"
		echo "AllowTcpForwarding no"
		echo "PermitTunnel no"
		echo "Port $port"
		echo "ListenAddress 127.0.0.1"
		echo "HostKey $work/server/host_key"
		echo "AuthorizedKeysFile $work/server/authorized_keys"
		echo "PidFile none"
		echo "UsePAM no"
		echo "StrictModes no"
		echo "HostKeyAlgorithms ssh-ed25519"
		echo "Ciphers chacha20-poly1305@openssh.com"
	} > "$work/server/$name.conf"
	timeout 600 "$sshd_program" -D -e -f "$work/server/$name.conf" > "$work/server/$name.log" 2>&1 &
	started=$!
	server_pids="$server_pids $started"
	if ! helper wait-port "$port" 10; then
		fail "sshd $name did not start (log $work/server/$name.log)"
		return 1
	fi
	if ! helper owner "$port" "$started"; then
		fail "port $port is not served by the test's sshd $name"
		return 1
	fi
	owned="$owned $port $started"
	return 0
}

# servers_owned: checks again, at the end, that every test port is still served by the test's own process.
servers_owned() {
	set -- $owned
	while [ $# -ge 2 ]; do
		if ! helper owner "$1" "$2"; then
			fail "port $1 is no longer served by the test's process $2"
		fi
		shift 2
	done
}

port_a=$(helper free-port)
port_rekey=$(helper free-port)
port_old=$(helper free-port)
port_password=$(helper free-port)
port_silent=$(helper free-port)
port_alive=$(helper free-port)
port_shell=$(helper free-port)
port_sh=$(helper free-port)
port_tries=$(helper free-port)
port_nosession=$(helper free-port)
port_notty=$(helper free-port)
port_closer=$(helper free-port)
port_closed=$(helper free-port)
start_sshd a "$port_a" "KexAlgorithms mlkem768x25519-sha256
PasswordAuthentication no" || { echo "interop-host: FAIL"; exit 1; }
start_sshd rekey "$port_rekey" "KexAlgorithms mlkem768x25519-sha256
RekeyLimit 1M
PasswordAuthentication no" || { echo "interop-host: FAIL"; exit 1; }
start_sshd old "$port_old" "KexAlgorithms curve25519-sha256" || { echo "interop-host: FAIL"; exit 1; }
start_sshd password "$port_password" "KexAlgorithms mlkem768x25519-sha256
PasswordAuthentication yes
PubkeyAuthentication no" || { echo "interop-host: FAIL"; exit 1; }
start_sshd alive "$port_alive" "KexAlgorithms mlkem768x25519-sha256
PasswordAuthentication no
ClientAliveInterval 1
ClientAliveCountMax 2" || { echo "interop-host: FAIL"; exit 1; }
start_sshd shell "$port_shell" "KexAlgorithms mlkem768x25519-sha256
PasswordAuthentication no
ForceCommand tty; exit 6" || { echo "interop-host: FAIL"; exit 1; }
start_sshd sh "$port_sh" "KexAlgorithms mlkem768x25519-sha256
PasswordAuthentication no
ForceCommand exec /bin/sh" || { echo "interop-host: FAIL"; exit 1; }
start_sshd tries "$port_tries" "KexAlgorithms mlkem768x25519-sha256
PasswordAuthentication no
MaxAuthTries 1" || { echo "interop-host: FAIL"; exit 1; }
start_sshd nosession "$port_nosession" "KexAlgorithms mlkem768x25519-sha256
PasswordAuthentication no
MaxSessions 0" || { echo "interop-host: FAIL"; exit 1; }
start_sshd notty "$port_notty" "KexAlgorithms mlkem768x25519-sha256
PasswordAuthentication no
PermitTTY no" || { echo "interop-host: FAIL"; exit 1; }
timeout 900 "$python" -I "$out/helper.py" silent "$port_silent" 890 &
server_pids="$server_pids $!"
helper wait-port "$port_silent" 10 || fail "the silent listener did not start"
helper owner "$port_silent" "$!" || { echo "interop-host: FAIL (port $port_silent is not the test's)"; exit 1; }
owned="$owned $port_silent $!"
timeout 900 "$python" -I "$out/helper.py" closer "$port_closer" 890 &
server_pids="$server_pids $!"
helper wait-port "$port_closer" 10 || fail "the closing listener did not start"
helper owner "$port_closer" "$!" || { echo "interop-host: FAIL (port $port_closer is not the test's)"; exit 1; }
owned="$owned $port_closer $!"

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

# No test asks for the user's own shell: a session without a command goes to a server whose ForceCommand runs /bin/sh
# or a fixed command, so no interactive login shell of the account runs (it would write the account's history).
# zssh ARGS...: zedBSD's ssh with the test's configuration and a time limit.
zssh() {
	timeout 60 "$ssh" -F "$config" "$@"
}

head -c 8388608 /dev/urandom > "$work/data8"

# The checks of peer A (INTEROP_PART=b leaves them out, for a run of peer B alone).
peer_a() {
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
echo 'echo shell-ran; exit 4' | zssh -o StrictHostKeyChecking=accept-new -p "$port_sh" 127.0.0.1 > "$work/t9.out" \
    2> "$work/t9.err"
expect_eq "a shell fed by stdin exit" "$?" 4
expect_eq "a shell fed by stdin output" "$(cat "$work/t9.out")" shell-ran
timeout 60 "$ssh" -F "$config" -o StrictHostKeyChecking=accept-new -N -p "$port_alive" 127.0.0.1 > /dev/null \
    2> "$work/t10.err" &
n_pid=$!
sleep 5
kill -0 "$n_pid" 2>/dev/null
check "-N answers the server's global keepalives and is still connected after 5 s" $?
kill -TERM "$n_pid" 2>/dev/null
wait "$n_pid"
expect_eq "-N runs until a signal and gives 255" "$?" 255
grep -q 'Killed by signal 15' "$work/t10.err"
check "-N reports the signal" $?

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

# (The server runs "tty; exit 6" in place of the shell, so no interactive shell of the user runs.)
helper pty "$work/pty7.out" 24 80 30 -- "$ssh" -F "$config" -o StrictHostKeyChecking=accept-new -p "$port_shell" \
    127.0.0.1
expect_eq "a shell request on the terminal (no command, stdin a terminal) exit" "$?" 6
grep -q '/dev/pts/' "$work/pty7.out"
check "without a command and with a terminal, the remote shell gets a terminal" $?
helper pty "$work/pty8.out" 24 80 30 -- "$ssh" -F "$config" -p "$port_a" 127.0.0.1 'tty; true'
expect_eq "a command on the terminal without -t exit" "$?" 0
grep -q 'not a tty' "$work/pty8.out"
check "with a command and without -t, no terminal is asked for" $?
echo 'tty' | zssh -t -p "$port_sh" 127.0.0.1 > "$work/t30.out" 2> "$work/t30.err"
expect_eq "-t with stdin not a terminal exit" "$?" 1
grep -q 'not a tty' "$work/t30.out"
check "-t with stdin not a terminal asks for no terminal" $?
grep -q 'Pseudo-terminal will not be allocated' "$work/t30.err"
check "-t with stdin not a terminal says so" $?

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
zssh -o ServerAliveInterval=1 -o ServerAliveCountMax=2 -p "$port_a" 127.0.0.1 'sleep 6; echo alive-ok' \
    > "$work/t19.out" 2> "$work/t19.err"
expect_eq "keepalives the server answers keep the session" "$?:$(cat "$work/t19.out")" "0:alive-ok"
start=$(date +%s)
zssh -o ServerAliveInterval=1 -o ServerAliveCountMax=2 -p "$port_a" 127.0.0.1 'kill -STOP $PPID; sleep 50' \
    > "$work/t20.out" 2> "$work/t20.err"
expect_eq "a server that stops answering keepalives gives 255" "$?" 255
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -le 8 ]
check "two unanswered keepalives a second apart end it in about 3 s (took ${elapsed} s)" $?
grep -q 'not responding' "$work/t20.err"
check "the timeout is reported" $?

# --- the server's keepalives (ClientAliveInterval) are answered.
zssh -o StrictHostKeyChecking=accept-new -p "$port_alive" 127.0.0.1 'sleep 6; echo client-alive-ok' > "$work/t29.out" \
    2> "$work/t29.err"
expect_eq "the server's keepalives are answered and the session lasts" "$?:$(cat "$work/t29.out")" "0:client-alive-ok"

# --- a server without the key exchange: "no matching", 255.
zssh -o StrictHostKeyChecking=accept-new -p "$port_old" 127.0.0.1 'echo reached' > "$work/t21.out" 2> "$work/t21.err"
expect_eq "a server with only curve25519-sha256 gives 255" "$?:$(cat "$work/t21.out")" "255:"
grep -q 'no matching key exchange method' "$work/t21.err"
check "the failed key exchange says no matching key exchange method" $?
crs=$(tr -cd '\r' < "$work/t21.err" | wc -c | tr -d ' ')
expect_eq "messages without a terminal end with LF alone" "$crs" 0

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
chmod 604 "$work/open_key"
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

chmod 640 "$work/open_key"
zssh -i "$work/open_key" -p "$port_a" 127.0.0.1 'echo reached' > "$work/t31.out" 2> "$work/t31.err"
expect_eq "a key file the group may read is not used" "$?:$(cat "$work/t31.out")" "255:"
helper pty "$work/pty9.out" 24 80 30 -- "$ssh" -F "$config" -o BatchMode=yes -p "$port_password" 127.0.0.1 'echo reached'
expect_eq "batch mode on a terminal gives 255" "$?" 255
grep -q 'password:' "$work/pty9.out"
expect_eq "batch mode asks no password even on a terminal" "$?" 1
"$keygen" -q -t ed25519 -N '' -C stranger2 -f "$work/stranger_key2" > /dev/null 2>&1
zssh -o StrictHostKeyChecking=accept-new -i "$work/stranger_key" -i "$work/stranger_key2" -p "$port_tries" 127.0.0.1 \
    'echo reached' > "$work/t32.out" 2> "$work/t32.err"
expect_eq "a server that ends the connection after too many tries gives 255" "$?:$(cat "$work/t32.out")" "255:"
grep -q "Received disconnect from 127.0.0.1 port $port_tries:" "$work/t32.err"
check "the server's DISCONNECT and its reason are shown" $?

# --- channels refused, terminals refused, connections cut.
zssh -o StrictHostKeyChecking=accept-new -p "$port_nosession" 127.0.0.1 'echo reached' > "$work/t33.out" \
    2> "$work/t33.err"
expect_eq "a refused session channel gives 255" "$?:$(cat "$work/t33.out")" "255:"
grep -q 'open failed' "$work/t33.err"
check "a refused session channel is reported" $?
zssh -o StrictHostKeyChecking=accept-new -tt -p "$port_notty" 127.0.0.1 'echo notty-ok; exit 5' < /dev/null \
    > "$work/t34.out" 2> "$work/t34.err"
expect_eq "a refused terminal: the command still runs" "$?:$(tr -d '\r' < "$work/t34.out")" "5:notty-ok"
grep -q 'PTY allocation request failed' "$work/t34.err"
check "a refused terminal is reported" $?
zssh -n -p "$port_a" 127.0.0.1 'kill -KILL $PPID; sleep 30' > "$work/t35.out" 2> "$work/t35.err"
expect_eq "a connection cut under a running command gives 255" "$?" 255
grep -q 'closed by remote host' "$work/t35.err"
check "a connection cut under a running command is reported" $?
zssh -p "$port_closer" 127.0.0.1 true > /dev/null 2> "$work/t36.err"
expect_eq "a server that closes at once gives 255" "$?" 255
grep -q "Connection closed by 127.0.0.1 port $port_closer" "$work/t36.err"
check "a server that closes at once is reported" $?
zssh -p "$port_closed" 127.0.0.1 true > /dev/null 2> "$work/t37.err"
expect_eq "a port nobody listens on gives 255" "$?" 255
grep -q "connect to host 127.0.0.1 port $port_closed: Connection refused" "$work/t37.err"
check "a refused connection is reported" $?

# --- the sshd processes are still serving after all of the above.
zssh -p "$port_a" 127.0.0.1 'echo still-serving' > "$work/t28.out" 2> "$work/t28.err"
expect_eq "the server still serves" "$?:$(cat "$work/t28.out")" "0:still-serving"

}
if [ "${INTEROP_PART:-all}" != b ]; then
	peer_a
fi

# ======== Peer B: zedBSD's sshd. ========
source_dir=${SSH_SOURCE_DIR:-userland/base/ssh}
b_modules=""
for file in "$source_dir"/ssh-*.c; do
	case "$file" in
	*-main.c|*/ssh-client-*.c) ;;
	*) b_modules="$b_modules $file" ;;
	esac
done
b_dir="$work/sshd-b"
# An RSA host key the server passes over (the image's configuration names one before the ssh-ed25519 key).
timeout 60 "$openssh_keygen" -q -t rsa -b 2048 -N '' -f "$work/server/rsa_key" > /dev/null 2>&1
mkdir -p "$b_dir/home/.ssh"
chmod 700 "$b_dir/home" "$b_dir/home/.ssh"
# shellcheck disable=SC2086
timeout 300 cc -std=gnu99 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I. -I"$source_dir" -I"$crypto_include" \
    $(ls "$source_dir"/sshd-*.c) userland/base/login/verify.c plan/ws336/tests/sshd-host-shim.c $b_modules \
    $crypto_lib -lcrypt -o "$b_dir/sshd" > "$out/sshd-b.build.log" 2>&1
check "zedBSD's sshd builds for the host with the test's shim" $?
cp "$HOME/.ssh/id_ed25519.pub" "$b_dir/home/.ssh/authorized_keys"
chmod 600 "$b_dir/home/.ssh/authorized_keys"
cat > "$b_dir/shell.sh" <<'EOF'
#!/bin/sh
# The test account's shell on peer B: a command runs under /bin/sh; a login prints that it ran and ends.
if [ "${1-}" = "-c" ]; then
	exec /bin/sh -c "$2"
fi
echo "login-shell"
exit 0
EOF
chmod 755 "$b_dir/shell.sh"
# The same server with no shell for the account: a session's command is refused.
noshell_dir="$work/sshd-noshell"
mkdir -p "$noshell_dir/home/.ssh"
chmod 700 "$noshell_dir/home" "$noshell_dir/home/.ssh"
cp "$b_dir/sshd" "$noshell_dir/sshd"
cp "$b_dir/home/.ssh/authorized_keys" "$noshell_dir/home/.ssh/authorized_keys"

# The same server once more, to be stopped while a session runs.
stop_dir="$work/sshd-stop"
mkdir -p "$stop_dir/home/.ssh"
chmod 700 "$stop_dir/home" "$stop_dir/home/.ssh"
cp "$b_dir/sshd" "$b_dir/shell.sh" "$stop_dir/"
cp "$b_dir/home/.ssh/authorized_keys" "$stop_dir/home/.ssh/authorized_keys"

port_b=$(helper free-port)
port_noshell=$(helper free-port)
port_stop=$(helper free-port)
b_config() {
	cat <<EOF
Port $1
ListenAddress 127.0.0.1
HostKey $work/server/host_key
AuthorizedKeysFile .ssh/authorized_keys
PasswordAuthentication no
LoginGraceTime 30
Subsystem sftp $out/bin/sftp-server
EOF
}
{ echo "HostKey $work/server/rsa_key"; b_config "$port_b"; } > "$b_dir/sshd_config"
b_config "$port_noshell" > "$noshell_dir/sshd_config"
b_config "$port_stop" > "$stop_dir/sshd_config"

# sshd -t: a usable configuration gives 0; an unknown keyword gives its line and 1.
timeout 30 "$b_dir/sshd" -t -f "$b_dir/sshd_config" > "$work/b-t1.out" 2>&1
expect_eq "sshd -t on a usable configuration" "$?:$(cat "$work/b-t1.out")" "0:"
{ echo "Port $port_b"; echo "PasswordAuthentication no"; echo "NoSuchKeyword yes"; } > "$work/b-bad_config"
timeout 30 "$b_dir/sshd" -t -f "$work/b-bad_config" > "$work/b-t2.out" 2>&1
expect_eq "sshd -t on an unknown keyword gives 1" "$?" 1
grep -q 'line 3:' "$work/b-t2.out"
check "sshd -t names the line ($(cat "$work/b-t2.out"))" $?
timeout 30 "$b_dir/sshd" -t -f "$b_dir/sshd_config" extra > "$work/b-t3.out" 2>&1
expect_eq "sshd -t with an operand gives 1" "$?" 1
{ echo "HostKey $work/server/rsa_key"; b_config "$port_b"; } > "$work/b-rsa_config"
timeout 30 "$b_dir/sshd" -t -f "$work/b-rsa_config" > "$work/b-t4.out" 2>&1
expect_eq "sshd -t passes over an RSA host key to the ssh-ed25519 one" "$?:$(cat "$work/b-t4.out")" "0:"
{ echo "HostKey $work/server/rsa_key"; echo "Port $port_b"; } > "$work/b-rsa-only_config"
timeout 30 "$b_dir/sshd" -t -f "$work/b-rsa-only_config" > "$work/b-t5.out" 2>&1
expect_eq "sshd -t with only an RSA host key gives 1" "$?" 1
grep -q 'no ssh-ed25519 host key' "$work/b-t5.out"
check "sshd -t says there is no ssh-ed25519 host key" $?
{ echo "HostKey $work/server/host_key"; echo "Port $port_b"; echo "NoSuchKeyword yes"; } > "$work/b-bad2_config"
timeout 30 "$b_dir/sshd" -t -f "$work/b-bad2_config" > "$work/b-t6.out" 2>&1
expect_eq "sshd -t on an unknown keyword after a usable host key gives 1" "$?" 1

# start_b NAME DIR PORT: starts zedBSD's sshd and fails closed unless the port is its own.
start_b() {
	timeout 600 "$2/sshd" -D -e -f "$2/sshd_config" > "$2/stderr" 2>&1 &
	b_pid=$!
	server_pids="$server_pids $b_pid"
	if ! helper wait-port "$3" 10; then
		fail "zedBSD's sshd $1 did not start ($(head -5 "$2/stderr"))"
		return 1
	fi
	if ! helper owner "$3" "$b_pid"; then
		fail "port $3 is not served by the test's sshd $1"
		return 1
	fi
	owned="$owned $3 $b_pid"
	return 0
}
start_b b "$b_dir" "$port_b" || { servers_owned; stop_servers; echo "interop-host: FAIL"; exit 1; }
b_listener=$(helper holder "$port_b" "$b_pid")
sleep 1
b_fds=$(helper fds "$b_listener")
start_b noshell "$noshell_dir" "$port_noshell" || { servers_owned; stop_servers; echo "interop-host: FAIL"; exit 1; }

# ossh ARGS...: the host's OpenSSH ssh with an empty environment, no configuration and the test's own files.
known_b="$work/known_b"
ossh() {
	timeout 60 env -i PATH="$PATH" "$openssh_client" -F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes \
	    -i "$HOME/.ssh/id_ed25519" -o UserKnownHostsFile="$known_b" -o GlobalKnownHostsFile=/dev/null \
	    -o StrictHostKeyChecking=accept-new -o ControlPath=none "$@"
}

# --- OpenSSH's ssh to zedBSD's sshd: exec, the exit status, a signal, no post-quantum warning.
ossh -p "$port_b" 127.0.0.1 'echo b-hello; exit 3' > "$work/b1.out" 2> "$work/b1.err"
expect_eq "B: exit 3 gives 3" "$?:$(cat "$work/b1.out")" "3:b-hello"
grep -qi 'post-quantum' "$work/b1.err"
expect_eq "B: no post-quantum warning" "$?" 1
expect_eq "B: the known_hosts line" "$(cat "$known_b" 2>/dev/null)" "[127.0.0.1]:$port_b ssh-ed25519 $host_blob"
ossh -p "$port_b" 127.0.0.1 'kill -TERM $$' > /dev/null 2> "$work/b2.err"
expect_eq "B: a command killed by a signal gives 255" "$?" 255

# --- 8 MiB through stdin and back.
ossh -p "$port_b" 127.0.0.1 cat < "$work/data8" > "$work/b-data8.out" 2> "$work/b3.err"
expect_eq "B: 8 MiB round trip exit" "$?" 0
cmp -s "$work/data8" "$work/b-data8.out"
check "B: 8 MiB round trip is the same bytes" $?

# --- -tt and the terminal size.
helper pty "$work/b-pty1.out" 37 123 30 -- /usr/bin/env -i PATH="$PATH" "$openssh_client" -F /dev/null -o BatchMode=yes \
    -o IdentitiesOnly=yes -i "$HOME/.ssh/id_ed25519" -o UserKnownHostsFile="$known_b" -o GlobalKnownHostsFile=/dev/null \
    -tt -p "$port_b" 127.0.0.1 'printf "SIZE %s\n" "$(stty size)"; tty'
expect_eq "B: -tt exit" "$?" 0
grep -q 'SIZE 37 123' "$work/b-pty1.out"
check "B: -tt gives the terminal the client's size" $?
grep -q '/dev/pts/' "$work/b-pty1.out"
check "B: -tt runs the command on a terminal" $?

# --- three sessions over one ControlMaster connection.
timeout 60 env -i PATH="$PATH" "$openssh_client" -F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes \
    -i "$HOME/.ssh/id_ed25519" -o UserKnownHostsFile="$known_b" -o GlobalKnownHostsFile=/dev/null \
    -o ControlMaster=yes -o ControlPath="$work/cm.sock" -o ControlPersist=no -N -p "$port_b" 127.0.0.1 \
    > /dev/null 2> "$work/b-cm.err" &
cm_pid=$!
server_pids="$server_pids $cm_pid"
tries=0
while [ ! -S "$work/cm.sock" ] && [ "$tries" -lt 100 ]; do
	sleep 0.1
	tries=$((tries + 1))
done
for session in 1 2 3; do
	timeout 60 env -i PATH="$PATH" "$openssh_client" -F /dev/null -o ControlMaster=no -o ControlPath="$work/cm.sock" \
	    -p "$port_b" 127.0.0.1 "sleep 1; echo session-$session" > "$work/b-cm$session.out" 2>&1 &
done
wait_sessions=0
for session in 1 2 3; do
	tries=0
	while ! grep -q "session-$session" "$work/b-cm$session.out" 2>/dev/null && [ "$tries" -lt 150 ]; do
		sleep 0.1
		tries=$((tries + 1))
	done
	grep -q "session-$session" "$work/b-cm$session.out" || wait_sessions=1
done
check "B: three sessions over one ControlMaster connection" "$wait_sessions"
timeout 30 env -i PATH="$PATH" "$openssh_client" -F /dev/null -o ControlPath="$work/cm.sock" -O exit -p "$port_b" \
    127.0.0.1 > /dev/null 2>&1
wait "$cm_pid" 2>/dev/null

# --- scp (the SFTP protocol) and sftp -b.
oscp() {
	timeout 60 env -i PATH="$PATH" /usr/bin/scp -F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes \
	    -i "$HOME/.ssh/id_ed25519" -o UserKnownHostsFile="$known_b" -o GlobalKnownHostsFile=/dev/null -P "$port_b" "$@"
}
oscp "$work/data8" 127.0.0.1:b-up8 > "$work/b-scp1.out" 2>&1
expect_eq "B: scp up exit" "$?" 0
oscp 127.0.0.1:b-up8 "$work/b-down8" > "$work/b-scp2.out" 2>&1
expect_eq "B: scp down exit" "$?" 0
cmp -s "$work/data8" "$work/b-down8"
check "B: scp 8 MiB both ways is the same bytes" $?
head -c 30000 /dev/urandom > "$work/b-small"
cat > "$work/b-sftp.batch" <<EOF
mkdir b-dir
put $work/b-small b-dir/one
rename b-dir/one b-dir/two
ls b-dir
get b-dir/two $work/b-small.back
EOF
timeout 60 env -i PATH="$PATH" /usr/bin/sftp -F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes \
    -i "$HOME/.ssh/id_ed25519" -o UserKnownHostsFile="$known_b" -o GlobalKnownHostsFile=/dev/null -P "$port_b" \
    -b "$work/b-sftp.batch" 127.0.0.1 > "$work/b-sftp.out" 2>&1
expect_eq "B: sftp -b exit" "$?" 0
grep -q 'b-dir/two' "$work/b-sftp.out"
check "B: sftp ls shows the renamed file" $?
cmp -s "$work/b-small" "$work/b-small.back"
check "B: sftp put, rename and get keep the bytes" $?

# --- zedBSD's sftp-server through zedBSD's sshd: scp -r of a directory, scp -p (times and modes), and the sftp
# requests one by one.
mkdir -p "$work/tree/sub/deeper"
printf 'one\n' > "$work/tree/one"
head -c 70000 /dev/urandom > "$work/tree/sub/two"
printf 'three\n' > "$work/tree/sub/deeper/three"
oscp -r "$work/tree" 127.0.0.1:b-tree > "$work/b-scp3.out" 2>&1
expect_eq "B: scp -r up exit" "$?" 0
oscp -r 127.0.0.1:b-tree "$work/tree-back" > "$work/b-scp4.out" 2>&1
expect_eq "B: scp -r down exit" "$?" 0
diff -r "$work/tree" "$work/tree-back" > /dev/null 2>&1
check "B: scp -r keeps the tree" $?
printf 'kept\n' > "$work/timed"
chmod 640 "$work/timed"
touch -d @1000000000 "$work/timed"
oscp -p "$work/timed" 127.0.0.1:b-timed > "$work/b-scp5.out" 2>&1
expect_eq "B: scp -p up exit" "$?" 0
expect_eq "B: scp -p up keeps the mode and the time" "$(stat -c '%a %Y' "$b_dir/home/b-timed" 2>/dev/null)" "640 1000000000"
chmod 604 "$b_dir/home/b-timed"
touch -d @1100000000 "$b_dir/home/b-timed"
oscp -p 127.0.0.1:b-timed "$work/timed-back" > "$work/b-scp6.out" 2>&1
expect_eq "B: scp -p down exit" "$?" 0
expect_eq "B: scp -p down keeps the mode and the time" "$(stat -c '%a %Y' "$work/timed-back" 2>/dev/null)" "604 1100000000"
cat > "$work/b-sftp2.batch" <<EOF
mkdir b-gone
rmdir b-gone
put $work/b-small b-file
symlink b-file b-symlink
-get b-missing $work/b-missing.back
-get b-noperm $work/b-noperm.back
rm b-file
EOF
printf 'secret\n' > "$b_dir/home/b-noperm"
chmod 000 "$b_dir/home/b-noperm"
timeout 60 env -i PATH="$PATH" /usr/bin/sftp -F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes \
    -i "$HOME/.ssh/id_ed25519" -o UserKnownHostsFile="$known_b" -o GlobalKnownHostsFile=/dev/null -P "$port_b" \
    -b "$work/b-sftp2.batch" 127.0.0.1 > "$work/b-sftp2.out" 2>&1
expect_eq "B: sftp -b of mkdir, rmdir, put, symlink, get, rm exit" "$?" 0
[ ! -e "$b_dir/home/b-gone" ]
check "B: sftp rmdir removed the directory" $?
expect_eq "B: sftp symlink makes the link point at its target" "$(readlink "$b_dir/home/b-symlink" 2>/dev/null)" b-file
[ ! -e "$b_dir/home/b-file" ] && [ -L "$b_dir/home/b-symlink" ]
check "B: sftp rm removed the file and left the link" $?
grep -q 'b-missing" not found' "$work/b-sftp2.out"
check "B: sftp get of a missing file says it is not found (NO_SUCH_FILE)" $?
grep -q 'Permission denied' "$work/b-sftp2.out"
check "B: sftp get of a file without permission says Permission denied" $?
chmod 600 "$b_dir/home/b-noperm"

# --- zedBSD's sftp-server on its own: the protocol (expected bytes from the draft), then 5000 runs of requests
# with short names in an empty directory, and 5000 runs of random bytes against a build whose file system calls all go
# to sftp-fs-stub.c.  That build fails closed: sftp-server-main.c may call no file system function the stub lacks.
# INTEROP_FUZZ_RUNS=<n> runs fewer of each kind (for the checks of a changed copy); 5000 of each is the phase's 10000.
fuzz_runs=${INTEROP_FUZZ_RUNS:-5000}
mkdir -p "$work/sftp-raw" "$work/sftp-fuzz"
"$python" -I plan/ws336/tests/sftp-host.py raw "$out/bin/sftp-server" "$work/sftp-raw" > "$work/sftp-raw.out" 2>&1
check "sftp-server: the protocol checks ($(grep -c '^ok' "$work/sftp-raw.out") ok, $(grep '^FAIL' "$work/sftp-raw.out" | tr '\n' ' '))" $?
cc -std=c89 -O1 -D_DEFAULT_SOURCE -I. -I"$source_dir" -I"$crypto_include" -c "$source_dir/sftp-server-main.c" \
    -o "$out/sftp-server-main.o" > "$out/sftp-stub.build.log" 2>&1
stub_allowed=" chmod chown close closedir fchmod fchown fstat ftruncate futimens getgrgid getpwuid link lstat mkdir open opendir pread pwrite readdir readlink realpath rename rmdir stat symlink truncate unlink utimensat __errno_location signal malloc free fwrite read write snprintf sprintf strlen strcmp strrchr strcpy memcpy memset memchr stderr time "
unexpected=""
for symbol in $(nm -u "$out/sftp-server-main.o" | awk '{print $2}'); do
	case "$symbol" in
	ssh_*) continue ;;
	esac
	case "$stub_allowed" in
	*" $symbol "*) ;;
	*) unexpected="$unexpected $symbol" ;;
	esac
done
expect_eq "sftp-server calls nothing the stub does not answer" "$unexpected" ""
stub_ok=1
if [ -z "$unexpected" ]; then
	# shellcheck disable=SC2086
	timeout 300 cc -std=gnu99 -O1 -g -Wall -Wextra -D_DEFAULT_SOURCE -fsanitize=address,undefined \
	    -fno-sanitize-recover=undefined -fno-omit-frame-pointer -I. -I"$source_dir" -I"$crypto_include" \
	    "$source_dir/sftp-server-main.c" plan/ws336/tests/sftp-fs-stub.c $b_modules $crypto_lib -o "$out/bin/sftp-server-stub" \
	    >> "$out/sftp-stub.build.log" 2>&1 && stub_ok=0
fi
check "the stubbed sftp-server builds" "$stub_ok"
"$python" -I plan/ws336/tests/sftp-host.py fuzz-structured "$out/bin/sftp-server" "$work/sftp-fuzz" "$fuzz_runs" \
    > "$work/sftp-fuzz1.out" 2>&1
check "sftp-server: $fuzz_runs runs of requests with short names ($(tail -1 "$work/sftp-fuzz1.out"))" $?
if [ "$stub_ok" = 0 ]; then
	mkdir -p "$work/sftp-stub"
	"$python" -I plan/ws336/tests/sftp-host.py fuzz-random "$out/bin/sftp-server-stub" "$work/sftp-stub" "$fuzz_runs" \
	    > "$work/sftp-fuzz2.out" 2>&1
	check "sftp-server: $fuzz_runs runs of random bytes ($(tail -1 "$work/sftp-fuzz2.out"))" $?
fi

# --- refused algorithms, and the server still serves.
ossh -o KexAlgorithms=curve25519-sha256 -p "$port_b" 127.0.0.1 'echo reached' > "$work/b4.out" 2> "$work/b4.err"
expect_eq "B: KexAlgorithms curve25519-sha256 is refused with 255" "$?:$(cat "$work/b4.out")" "255:"
ossh -o Ciphers=aes128-ctr -p "$port_b" 127.0.0.1 'echo reached' > "$work/b5.out" 2> "$work/b5.err"
expect_eq "B: Ciphers aes128-ctr is refused with 255" "$?:$(cat "$work/b5.out")" "255:"
ossh -p "$port_b" 127.0.0.1 'echo after-refusals' > "$work/b6.out" 2> "$work/b6.err"
expect_eq "B: the server serves after the refusals" "$?:$(cat "$work/b6.out")" "0:after-refusals"

# --- -X and -A: the requests are refused and the session goes on.
DISPLAY=:99 timeout 60 env -i PATH="$PATH" DISPLAY=:99 "$openssh_client" -F /dev/null -o BatchMode=yes \
    -o IdentitiesOnly=yes -i "$HOME/.ssh/id_ed25519" -o UserKnownHostsFile="$known_b" -o GlobalKnownHostsFile=/dev/null \
    -X -p "$port_b" 127.0.0.1 'echo x-ok' > "$work/b7.out" 2> "$work/b7.err"
expect_eq "B: -X: the session goes on" "$?:$(cat "$work/b7.out")" "0:x-ok"
grep -q 'X11 forwarding request failed' "$work/b7.err"
check "B: -X: x11-req is refused" $?
timeout 30 ssh-agent -a "$work/agent.sock" -s > "$work/agent.env" 2>&1
agent_pid=$(sed -n 's/^SSH_AGENT_PID=\([0-9]*\);.*/\1/p' "$work/agent.env")
[ -n "$agent_pid" ] && server_pids="$server_pids $agent_pid"
timeout 60 env -i PATH="$PATH" SSH_AUTH_SOCK="$work/agent.sock" "$openssh_client" -F /dev/null -o BatchMode=yes \
    -o IdentitiesOnly=yes -i "$HOME/.ssh/id_ed25519" -o UserKnownHostsFile="$known_b" -o GlobalKnownHostsFile=/dev/null \
    -A -v -p "$port_b" 127.0.0.1 'echo a-ok; echo "S=${SSH_AUTH_SOCK-unset}"' > "$work/b8.out" 2> "$work/b8.err"
expect_eq "B: -A: the session goes on without an agent" "$?:$(tr '\n' ' ' < "$work/b8.out")" "0:a-ok S=unset "
grep -q 'Requesting authentication agent forwarding' "$work/b8.err"
check "B: -A: the client asked for agent forwarding (auth-agent-req@openssh.com)" $?

# --- a shell that cannot be executed: the command is refused (OpenSSH's ssh and zedBSD's).
ossh -p "$port_noshell" 127.0.0.1 'echo reached' > "$work/b9.out" 2> "$work/b9.err"
expect_eq "B: a shell that cannot run: OpenSSH's ssh gives 255" "$?:$(cat "$work/b9.out")" "255:"
grep -q 'exec request failed' "$work/b9.err"
check "B: a shell that cannot run: the exec request is refused" $?
grep -q 'cannot be executed' "$noshell_dir/log"
check "B: a shell that cannot run: the server says why" $?

# --- zedBSD's ssh to zedBSD's sshd.
zssh -o StrictHostKeyChecking=accept-new -p "$port_b" 127.0.0.1 'echo z-hello; exit 4' > "$work/z1.out" 2> "$work/z1.err"
expect_eq "ours: exit 4 gives 4" "$?:$(cat "$work/z1.out")" "4:z-hello"
zssh -p "$port_b" 127.0.0.1 cat < "$work/data8" > "$work/z-data8.out" 2> "$work/z2.err"
expect_eq "ours: 8 MiB round trip exit" "$?" 0
cmp -s "$work/data8" "$work/z-data8.out"
check "ours: 8 MiB round trip is the same bytes" $?
zssh -p "$port_b" 127.0.0.1 'kill -TERM $$' > /dev/null 2>&1
expect_eq "ours: a command killed by a signal gives 255" "$?" 255
helper pty "$work/z-pty1.out" 37 123 30 -- "$ssh" -F "$config" -tt -p "$port_b" 127.0.0.1 'printf "SIZE %s\n" "$(stty size)"'
expect_eq "ours: -tt exit" "$?" 0
grep -q 'SIZE 37 123' "$work/z-pty1.out"
check "ours: -tt gives the terminal the client's size" $?
zssh -o StrictHostKeyChecking=accept-new -p "$port_noshell" 127.0.0.1 'echo reached' > "$work/z3.out" 2> "$work/z3.err"
expect_eq "ours: a refused exec gives 255" "$?:$(cat "$work/z3.out")" "255:"
grep -q 'exec request failed on channel' "$work/z3.err"
check "ours: a refused exec is reported ($(cat "$work/z3.err"))" $?

# --- MaxStartups (10): ten connections that send nothing, the eleventh closed; a login frees a place.
helper startups "$port_b" 10 "$b_dir/log" -- "$ssh" -F "$config" -p "$port_b" 127.0.0.1 'sleep 8' \
    > "$work/b-startups.out" 2>&1
expect_eq "B: MaxStartups ($(cat "$work/b-startups.out"))" "$?" 0
zssh -p "$port_b" 127.0.0.1 'echo after-startups' > "$work/b10.out" 2> "$work/b10.err"
expect_eq "B: the server serves after MaxStartups" "$?:$(cat "$work/b10.out")" "0:after-startups"

# --- the listener keeps nothing of the connections it handed to monitors (the same descriptors as at its start),
# and the monitors told the log their peer.
sleep 2
expect_eq "B: the listener's descriptors after every connection" "$(helper fds "$b_listener")" "$b_fds"
grep -q 'connection from 127\.0\.0\.1 [0-9]* ended before a session' "$b_dir/log"
check "B: a monitor names its peer in the log" $?

# --- a listener stopped while a session runs: the session goes on, the port is closed.
start_b stop "$stop_dir" "$port_stop" || fail "the sshd to stop did not start"
stop_listener=$(helper holder "$port_stop" "$b_pid")
zssh -o StrictHostKeyChecking=accept-new -p "$port_stop" 127.0.0.1 'sleep 4; echo survived' > "$work/b11.out" \
    2> "$work/b11.err" &
session_pid=$!
tries=0
while ! grep -q 'Accepted login' "$stop_dir/log" 2>/dev/null && [ "$tries" -lt 300 ]; do
	sleep 0.1
	tries=$((tries + 1))
done
kill -TERM "$stop_listener"
sleep 0.5
"$python" -I -c 'import socket, sys
try:
    socket.create_connection(("127.0.0.1", int(sys.argv[1])), 2).close()
except OSError:
    sys.exit(0)
sys.exit(1)' "$port_stop"
check "B: a stopped listener's port takes no connection (no monitor holds it)" $?
wait "$session_pid"
expect_eq "B: the session goes on after the listener stopped" "$?:$(cat "$work/b11.out")" "0:survived"
owned_left=""
set -- $owned
while [ $# -ge 2 ]; do
	[ "$1" = "$port_stop" ] || owned_left="$owned_left $1 $2"
	shift 2
done
owned=$owned_left

# --- no sanitizer report in any of zedBSD's sshd processes (their standard error is the server's file).
grep -l 'ERROR: AddressSanitizer\|runtime error:' "$b_dir/stderr" "$noshell_dir/stderr" "$stop_dir/stderr" > /dev/null 2>&1
expect_eq "B: no sanitizer report from zedBSD's sshd" "$?" 1

servers_owned
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
