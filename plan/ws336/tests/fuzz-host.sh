#!/bin/sh
# The adversarial checks of zedBSD's ssh and sshd (userland/base/ssh) on the host, with ASan and UBSan:
#  1. libFuzzer targets (host clang's -fsanitize=fuzzer, no toolchain change) for every parser and the protocol layers:
#     fuzz/fuzz-common.c (name-lists, base64, the RFC 4251 reader), fuzz-kex.c (KEXINIT, the choice, the hybrid
#     messages), fuzz-key.c (blobs, lines, private key files, signatures), fuzz-auth.c (service and user
#     authentication messages), fuzz-channel.c (a channel table fed message runs), fuzz-config.c (sshd_config,
#     ssh_config, -o), fuzz-hostfile.c (authorized_keys, known_hosts, patterns), fuzz-sftp.c (SFTP packets and
#     requests), fuzz-privsep.c (the monitor's frames), fuzz-transport.c (a server or client transport fed received
#     bytes; seeds hold an identification line and a KEXINIT), fuzz-monitor.c (sshd_monitor_handle answering a
#     compromised child, with the account checks replaced so nothing reaches the host's accounts or files, and the
#     invariant that no login is granted without the authorized key's signature or the secret password).  Each runs
#     FUZZ_RUNS (default 3000000, at least 1000000) inputs; a crash, a sanitizer report, a timeout or a leak fails it.
#  2. sftp-server as a program: random bytes against the build whose file system calls all go to sftp-fs-stub.c
#     (fails closed when the server calls a file system function the stub does not answer), and requests with only
#     short "/"-free names in an empty directory (sftp-host.py), SFTP_RUNS each (default 10000).
#  3. Timing (timing-host.c): the monitor's answers for an existing account with the right, a wrong and an empty
#     password, a locked account, no account, and key queries for an account and for none, all within 50 ms.
#  4. Descriptors: zedBSD's sshd (with the test shim of interop-host.sh, on 127.0.0.1, checked to be the test's own
#     process) runs a command and a terminal session for the host's OpenSSH ssh; the command's process holds only
#     its three streams, none of them a socket (no monitor socket, no listener, no connection).
# Nothing outside the run directory is touched: the fuzz targets open no file (the monitor's account checks are
# replaced), the random SFTP input reaches only the stub, sshd serves the test's account with the run directory's
# home and shell.
#   sh plan/ws336/tests/fuzz-host.sh
# Last line: fuzz-host: PASS or fuzz-host: FAIL.  Output under build/tmp/fuzz-host (a fresh run directory).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
fresh_out build/tmp/fuzz-host
out=$(cd build/tmp/fuzz-host && pwd -P)
python=/usr/bin/python3
fuzz_runs=${FUZZ_RUNS:-3000000}
sftp_runs=${SFTP_RUNS:-10000}
status=0
fail() {
	echo "FAIL $*"
	status=1
}
ASAN_OPTIONS=detect_leaks=1:exitcode=99
UBSAN_OPTIONS=halt_on_error=1:exitcode=99
export ASAN_OPTIONS UBSAN_OPTIONS
source_dir=${SSH_SOURCE_DIR:-userland/base/ssh}
modules=""
for file in "$source_dir"/ssh-*.c; do
	case "$file" in
	*-main.c|*/ssh-client-*.c) ;;
	*) modules="$modules $file" ;;
	esac
done
mkdir -p "$out/bin" "$out/corpus" "$out/work"

# --- 1. the libFuzzer targets.
targets="common kex key auth channel config hostfile sftp privsep transport monitor"
for target in $targets; do
	extra=""
	[ "$target" = monitor ] && extra="$source_dir/sshd-monitor.c"
	# shellcheck disable=SC2086
	if ! timeout 600 clang -std=gnu99 -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	    -D_DEFAULT_SOURCE -I. -I"$source_dir" -Iplan/ws336/tests/host-crypto "plan/ws336/tests/fuzz/fuzz-$target.c" \
	    $extra $modules -lcrypto -o "$out/bin/fuzz-$target" > "$out/fuzz-$target.build.log" 2>&1; then
		fail "build of fuzz-$target ($(grep error "$out/fuzz-$target.build.log" | head -2))"
	fi
	mkdir -p "$out/corpus/$target"
done

# The seeds: the transport's (a role byte, an identification line, an unkeyed KEXINIT), and a few for the others.
"$python" -I - "$out/corpus" <<'EOF'
import os, struct, sys
root = sys.argv[1]
def string(b):
    if isinstance(b, str): b = b.encode()
    return struct.pack(">I", len(b)) + b
def packet(payload):
    pad = 8 - ((5 + len(payload)) % 8)
    if pad < 4: pad += 8
    return struct.pack(">IB", 1 + len(payload) + pad, pad) + payload + b"\0" * pad
def kexinit(side):
    strict = "kex-strict-s-v00@openssh.com" if side == "s" else "kex-strict-c-v00@openssh.com"
    lists = ["mlkem768x25519-sha256," + strict, "ssh-ed25519", "chacha20-poly1305@openssh.com",
             "chacha20-poly1305@openssh.com", "", "", "none", "none", "", ""]
    return bytes([20]) + b"\x11" * 16 + b"".join(string(x) for x in lists) + b"\0" + b"\0\0\0\0"
def write(target, name, data):
    open(os.path.join(root, target, name), "wb").write(data)
write("transport", "server", b"\x01" + b"SSH-2.0-seed\r\n" + packet(kexinit("c")) + packet(bytes([30]) + string(b"\x22" * 1216)))
write("transport", "client", b"\x00" + b"SSH-2.0-seed\r\n" + packet(kexinit("s")))
write("kex", "kexinit", kexinit("c"))
write("auth", "request", bytes([50]) + string("user") + string("ssh-connection") + string("publickey") + b"\x01" +
      string("ssh-ed25519") + string(string("ssh-ed25519") + string(b"\x07" * 32)) + string(string("ssh-ed25519") + string(b"\x08" * 64)))
opened = bytes([90]) + string("session") + struct.pack(">III", 0, 65536, 32768)
write("channel", "open", struct.pack(">H", len(opened)) + opened)
write("config", "sshd", b"\x01Port 22\nHostKey /etc/ssh/key\nMaxAuthTries 3\nSubsystem sftp /x\n")
write("hostfile", "line", b"[h]:2222 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMD\n")
write("sftp", "open", struct.pack(">I", 21) + bytes([3]) + struct.pack(">I", 7) + string("file") + struct.pack(">II", 3, 0))
write("privsep", "frame", struct.pack(">IB", 37, 1) + string(b"\x01" * 32))
write("monitor", "login", bytes([1, 4]) + struct.pack(">H", 200) + string("fuzzuser") + string(string("ssh-ed25519") + string(b"\x07" * 32)) +
      string(b"\x00" * 40) + string(b"\x00" * 83))
EOF

for target in $targets; do
	[ -x "$out/bin/fuzz-$target" ] || continue
	timeout 1800 "$out/bin/fuzz-$target" -runs="$fuzz_runs" -max_total_time=1500 -rss_limit_mb=4096 -timeout=30 \
	    -artifact_prefix="$out/work/$target-" "$out/corpus/$target" > "$out/fuzz-$target.log" 2>&1
	rc=$?
	done_runs=$(sed -n 's/^Done \([0-9]*\) runs.*/\1/p' "$out/fuzz-$target.log" | tail -1)
	echo "fuzz-$target: exit $rc, ${done_runs:-0} runs, $(grep -c '' "$out/fuzz-$target.log") log lines"
	if [ "$rc" != 0 ]; then
		fail "fuzz-$target ($(grep -E 'ERROR|SUMMARY|abort|granted|deadly' "$out/fuzz-$target.log" | head -3 | tr '\n' ' '))"
	elif [ "${done_runs:-0}" -lt 1000000 ]; then
		fail "fuzz-$target ran only ${done_runs:-0} inputs"
	fi
done

# --- 2. sftp-server as a program: random bytes only to the stubbed build (which fails closed), short names in an
# empty directory for the real one.
SSH_SOURCE_DIR=$source_dir sh plan/ws336/tests/build-host.sh "$out/bin" sftp-server > "$out/sftp-server.build.log" 2>&1 ||
    fail "build of sftp-server"
cc -std=c89 -O1 -D_DEFAULT_SOURCE -I. -I"$source_dir" -Iplan/ws336/tests/host-crypto -c "$source_dir/sftp-server-main.c" \
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
[ -z "$unexpected" ] || fail "sftp-server calls file system functions the stub does not answer:$unexpected"
if [ -z "$unexpected" ]; then
	# shellcheck disable=SC2086
	timeout 300 cc -std=gnu99 -O1 -g -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	    -I. -I"$source_dir" -Iplan/ws336/tests/host-crypto "$source_dir/sftp-server-main.c" plan/ws336/tests/sftp-fs-stub.c \
	    $modules -lcrypto -o "$out/bin/sftp-server-stub" >> "$out/sftp-stub.build.log" 2>&1 || fail "build of the stubbed sftp-server"
	mkdir -p "$out/work/sftp-stub"
	"$python" -I plan/ws336/tests/sftp-host.py fuzz-random "$out/bin/sftp-server-stub" "$out/work/sftp-stub" "$sftp_runs" \
	    > "$out/sftp-random.log" 2>&1 || fail "sftp-server random: $(grep FAIL "$out/sftp-random.log" | head -2 | tr '\n' ' ')"
	echo "sftp-server random: $(tail -1 "$out/sftp-random.log")"
fi
mkdir -p "$out/work/sftp-names"
"$python" -I plan/ws336/tests/sftp-host.py fuzz-structured "$out/bin/sftp-server" "$out/work/sftp-names" "$sftp_runs" \
    > "$out/sftp-names.log" 2>&1 || fail "sftp-server names: $(grep FAIL "$out/sftp-names.log" | head -2 | tr '\n' ' ')"
echo "sftp-server names: $(tail -1 "$out/sftp-names.log")"

# --- 3. timing.
# shellcheck disable=SC2086
if timeout 300 cc -std=gnu99 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -I. -I"$source_dir" -Iplan/ws336/tests/host-crypto \
    plan/ws336/tests/timing-host.c "$source_dir/sshd-monitor.c" "$source_dir/sshd-login.c" userland/base/login/verify.c \
    $modules -lcrypto -lcrypt -o "$out/bin/timing-host" > "$out/timing.build.log" 2>&1; then
	timeout 600 "$out/bin/timing-host" 5 > "$out/timing.log" 2>&1
	rc=$?
	cat "$out/timing.log"
	[ "$rc" = 0 ] || fail "timing"
else
	fail "build of timing-host"
fi

# --- 4. descriptors of a command and of a terminal session under zedBSD's sshd.
b=$out/work/sshd
mkdir -p "$b/home/.ssh"
chmod 700 "$b/home" "$b/home/.ssh"
# shellcheck disable=SC2086
timeout 300 cc -std=gnu99 -O1 -g -Wall -Wextra -D_DEFAULT_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I. -I"$source_dir" -Iplan/ws336/tests/host-crypto $(ls "$source_dir"/sshd-*.c) userland/base/login/verify.c \
    plan/ws336/tests/sshd-host-shim.c $modules -lcrypto -lcrypt -o "$b/sshd" > "$out/sshd.build.log" 2>&1 || fail "build of sshd"
ssh-keygen -q -t ed25519 -N '' -f "$b/host_key" > /dev/null 2>&1
ssh-keygen -q -t ed25519 -N '' -f "$out/work/id" > /dev/null 2>&1
cp "$out/work/id.pub" "$b/home/.ssh/authorized_keys"
chmod 600 "$b/home/.ssh/authorized_keys"
printf '#!/bin/sh\nif [ "${1-}" = "-c" ]; then exec /bin/sh -c "$2"; fi\nexit 0\n' > "$b/shell.sh"
chmod 755 "$b/shell.sh"
port=$("$python" -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
printf 'Port %s\nListenAddress 127.0.0.1\nHostKey %s/host_key\nPasswordAuthentication no\n' "$port" "$b" > "$b/sshd_config"
timeout 300 "$b/sshd" -D -e -f "$b/sshd_config" > "$b/stderr" 2>&1 &
sshd_pid=$!
if "$python" -I - "$port" "$sshd_pid" <<'EOF'
import os, socket, sys, time
port, root = int(sys.argv[1]), int(sys.argv[2])
end = time.time() + 15
while time.time() < end:
    try:
        socket.create_connection(("127.0.0.1", port), 1).close()
        break
    except OSError:
        time.sleep(0.1)
inodes = set("socket:[%s]" % l.split()[9] for l in open("/proc/net/tcp").read().splitlines()[1:]
             if l.split()[3] == "0A" and l.split()[1] == "0100007F:%04X" % port)
kids = {}
for e in os.listdir("/proc"):
    if e.isdigit():
        try:
            kids.setdefault(int(open("/proc/%s/stat" % e).read().rsplit(")", 1)[1].split()[1]), []).append(int(e))
        except OSError:
            pass
todo = [root]
while todo:
    p = todo.pop()
    todo.extend(kids.get(p, []))
    try:
        if inodes & set(os.readlink("/proc/%d/fd/%s" % (p, f)) for f in os.listdir("/proc/%d/fd" % p)):
            sys.exit(0)
    except OSError:
        pass
sys.exit(1)
EOF
then
	opts="-F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes -i $out/work/id -o UserKnownHostsFile=$b/known -o GlobalKnownHostsFile=/dev/null -o StrictHostKeyChecking=accept-new"
	# shellcheck disable=SC2086
	timeout 60 env -i PATH="$PATH" ssh $opts -p "$port" 127.0.0.1 'ls -l /proc/$$/fd | awk "NR > 1 { print \$9, \$11 }"' \
	    > "$out/work/fd-command.out" 2> "$out/work/fd-command.err" || fail "descriptors: the command did not run"
	# shellcheck disable=SC2086
	timeout 60 env -i PATH="$PATH" ssh $opts -tt -p "$port" 127.0.0.1 'ls -l /proc/$$/fd | awk "NR > 1 { print \$9, \$11 }"' \
	    < /dev/null > "$out/work/fd-terminal.out" 2> "$out/work/fd-terminal.err" || fail "descriptors: the terminal session did not run"
	tr -d '\r' < "$out/work/fd-terminal.out" > "$out/work/fd-terminal.txt"
	[ "$(cut -d ' ' -f 1 "$out/work/fd-command.out" | tr '\n' ' ')" = "0 1 2 " ] || fail "descriptors of a command: $(cat "$out/work/fd-command.out")"
	grep -q 'socket:' "$out/work/fd-command.out" && fail "a command holds a socket: $(cat "$out/work/fd-command.out")"
	[ "$(grep -c 'pipe:' "$out/work/fd-command.out")" = 3 ] || fail "a command's streams are not its three pipes"
	[ "$(cut -d ' ' -f 1 "$out/work/fd-terminal.txt" | tr '\n' ' ')" = "0 1 2 " ] || fail "descriptors of a terminal session: $(cat "$out/work/fd-terminal.txt")"
	grep -q 'socket:' "$out/work/fd-terminal.txt" && fail "a terminal session holds a socket"
	[ "$(grep -c '/dev/pts/' "$out/work/fd-terminal.txt")" = 3 ] || fail "a terminal session's streams are not its terminal"
	echo "descriptors: a command has $(tr '\n' ' ' < "$out/work/fd-command.out"); a terminal session has $(tr '\n' ' ' < "$out/work/fd-terminal.txt")"
else
	fail "port $port is not the test's sshd"
fi
kill -TERM "$sshd_pid" 2>/dev/null
wait "$sshd_pid" 2>/dev/null
grep -q 'ERROR: AddressSanitizer\|runtime error:' "$b/stderr" && fail "a sanitizer report from sshd"

sync
if [ "$status" = 0 ]; then echo "fuzz-host: PASS"; else echo "fuzz-host: FAIL"; fi
exit "$status"
