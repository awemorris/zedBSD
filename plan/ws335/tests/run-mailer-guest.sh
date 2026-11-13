#!/bin/sh
# Mail's TLS over libssl-compat on zedBSD (x86_64, QEMU): Mail's own tls.c, built with -DML_TLS_COMPAT as the image's
# mailer is, connects from the guest to the host's openssl s_server (-rev) with a pin, and a pin that is not the
# server's is refused.
#  1. The image: the platform's programs without the firmware packages (CONFIG_USERLAND_PROGRAMS on the make command
#     line: the defaults are read after the configuration file, so the file cannot take them out), Mail built with
#     -DML_TLS_COMPAT (STANDALONE_CFLAGS_mailer on the command line, what the mailer Makefile's proposed line does),
#     plan/ws335/tests/config-p036-guest.mk with the graphical login off.  IMAGE=<file> skips the build; BUILD_JOBS
#     (default 4) is passed to make.
#  2. The guest program: plan/ws335/tests/mailer-tls.c and userland/desktop/mailer/tls.c compiled with the flags the
#     image's mailer was compiled with (build/x86_64/packages/mailer/.flags, which must hold -DML_TLS_COMPAT) and
#     linked as a program of the image is; it goes into the guest over the serial line in base64 (checked by cksum).
#  3. The host's servers on 127.0.0.1 (10.0.2.2 from the guest): a self-signed certificate for localhost (TLS 1.3,
#     P-256), another self-signed one over TLS 1.2 (RSA), and a certificate the tests' CA signed.
#  4. In the guest (MAILER_TLS_ADDRESS=10.0.2.2, the name localhost): /bin/mailer names libssl-compat.so and no
#     OpenSSL library; without a pin ML_ERROR_UNTRUSTED with the fingerprint openssl x509 -fingerprint -sha256 gives;
#     with it the connection is made and a line goes each way; with another certificate's pin ML_ERROR_UNTRUSTED;
#     the same over TLS 1.2; the CA-signed server with the CA file (sent over the serial line) without a pin.
# The verdict is the guest program's own last line over the serial line (plan/tools/guest/serial.py), not a console
# log.  The QEMU run is named ws335-p2-p041, its directory build/tmp/ws335-mailer-guest (a fresh run); the disk is
# opened with snapshot=on.
#   sh plan/ws335/tests/run-mailer-guest.sh
# The last line is run-mailer-guest: PASS or run-mailer-guest: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
PATH=/usr/bin:/bin:$PATH
export PATH
. plan/tools/fresh-out.sh
out=build/tmp/ws335-mailer-guest
fresh_out "$out"
python=/usr/bin/python3
status=0
servers=
fail() {
	echo "run-mailer-guest: FAIL ($1)"
	exit 1
}

# 1. The image.
image=${IMAGE:-}
if [ -z "$image" ]; then
	programs=$("$python" -I - mk/platform-x86_64/defaults.mk <<'EOF'
import sys
names, inside = [], False
for line in open(sys.argv[1]):
    text = line.rstrip("\n")
    if not inside and text.startswith("CONFIG_USERLAND_PROGRAMS") and "+=" in text:
        inside = True
        text = text.split("+=", 1)[1]
    if inside:
        go_on = text.rstrip().endswith("\\")
        names += text.rstrip().rstrip("\\").split()
        if not go_on:
            break
print(" ".join(name for name in names if not name.endswith("-firmware")))
EOF
)
	[ -n "$programs" ] || fail "no program list from mk/platform-x86_64/defaults.mk"
	echo "run-mailer-guest: $(echo $programs | wc -w) programs, no firmware"
	if ! flock /tmp/zedbsd-image-build.lock timeout 7000 make CONFIGMK=plan/ws335/tests/config-p036-guest.mk \
	    CONFIG_GRAPHICAL_LOGIN=n BUILD_JOBS=${BUILD_JOBS:-4} 'STANDALONE_CFLAGS_mailer=$(STANDALONE_PROGRAM_CFLAGS) -DML_TLS_COMPAT' \
	    CONFIG_USERLAND_PROGRAMS="$programs" disk > "$out/make.log" 2>&1; then
		tail -20 "$out/make.log"
		fail "make disk, $out/make.log"
	fi
	sync
	# The warnings of the build, less those of noct (the toolchain's, not this test's to judge).
	noct=$(grep 'warning:' "$out/make.log" | grep -c 'userland/base/noct/')
	if grep -n 'warning:' "$out/make.log" | grep -v 'userland/base/noct/' | head -20 | grep .; then
		echo "run-mailer-guest: the image build warned (above)"
		status=1
	fi
	other=$(grep 'warning:' "$out/make.log" | grep -vc 'userland/base/noct/')
	echo "run-mailer-guest: the image is built; warnings: $other outside noct, $noct of noct"
	image=build/x86_64/hdd-image.img
fi
[ -f "$image" ] || fail "no image $image"

# 2. The guest program, with the image's mailer's flags.
flags=$(cat build/x86_64/packages/mailer/.flags 2> /dev/null)
case "$flags" in
*-DML_TLS_COMPAT*) ;;
*) fail "the image's mailer was not compiled with -DML_TLS_COMPAT (build/x86_64/packages/mailer/.flags)" ;;
esac
sysroot=$top/build/x86_64/sysroot
dynamic=$top/build/x86_64/dynamic
{
	eval "$flags -Wall -Wextra -Werror -c plan/ws335/tests/mailer-tls.c -o $out/mailer-tls.o" &&
	eval "$flags -Wall -Wextra -Werror -c userland/desktop/mailer/tls.c -o $out/tls.o" &&
	build/llvm/bin/clang --target=x86_64-unknown-zedbsd --sysroot="$sysroot" -m64 -march=x86-64 -nostdlib -pie \
	    -Wl,--no-relax -Wl,--gc-sections -Wl,--hash-style=gnu,-z,now,-z,relro,-z,separate-code \
	    -Wl,-z,stack-size=0x100000,--allow-shlib-undefined -Wl,--dynamic-linker=/lib/ld.so \
	    "$sysroot/usr/lib/crt1.o" "$out/mailer-tls.o" "$out/tls.o" -L"$dynamic" -Wl,-rpath-link,"$dynamic" \
	    -l:libkeiland.so -l:libm.so -l:libc.so -o "$out/mailer-tls"
} > "$out/guest-build.log" 2>&1
if [ ! -x "$out/mailer-tls" ] || [ -s "$out/guest-build.log" ]; then
	head -20 "$out/guest-build.log"
	fail "the guest program, $out/guest-build.log"
fi

# 3. The certificates and the servers.
pki=$out/pki
mkdir -p "$pki"
san=subjectAltName=DNS:localhost
{
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj /CN=localhost \
	    -addext "$san" -keyout "$pki/self.key" -out "$pki/self.pem" &&
	openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=localhost \
	    -addext "$san" -keyout "$pki/self12.key" -out "$pki/self12.pem" &&
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj /CN=localhost \
	    -addext "$san" -keyout "$pki/other.key" -out "$pki/other.pem" &&
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj /CN=mailer-tls-ca \
	    -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign \
	    -keyout "$pki/ca.key" -out "$pki/ca.pem" &&
	openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj /CN=localhost \
	    -keyout "$pki/leaf.key" -out "$pki/leaf.csr" &&
	printf '%s\nbasicConstraints=CA:FALSE\n' "$san" > "$pki/leaf.ext" &&
	openssl x509 -req -in "$pki/leaf.csr" -CA "$pki/ca.pem" -CAkey "$pki/ca.key" -CAcreateserial -days 2 \
	    -extfile "$pki/leaf.ext" -out "$pki/leaf.pem"
} > "$pki/openssl.log" 2>&1 || fail "certificates, $pki/openssl.log"
fingerprint() {
	openssl x509 -in "$1" -noout -fingerprint -sha256 | sed 's/^.*=//; s/://g' | tr 'A-F' 'a-f'
}
fp_self=$(fingerprint "$pki/self.pem")
fp_self12=$(fingerprint "$pki/self12.pem")
fp_other=$(fingerprint "$pki/other.pem")
fp_leaf=$(fingerprint "$pki/leaf.pem")
free_port() {
	"$python" -I -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])'
}
wait_port() {
	"$python" -I -c 'import socket, sys, time
for _ in range(100):
    try:
        socket.create_connection(("127.0.0.1", int(sys.argv[1])), 1).close()
        sys.exit(0)
    except OSError:
        time.sleep(0.1)
sys.exit(1)' "$1"
}
qemu=
trap 'kill $qemu $servers 2> /dev/null; wait $qemu 2> /dev/null; sync' EXIT
start_server() {
	port=$(free_port)
	timeout 3000 openssl s_server -rev -accept "127.0.0.1:$port" -cert "$pki/$1.pem" -key "$pki/$1.key" $2 \
	    > "$out/server-$1.log" 2>&1 < /dev/null &
	servers="$servers $!"
	wait_port "$port" || fail "s_server $1 did not start"
	eval "port_$1=$port"
}
start_server self ""
start_server self12 "-tls1_2"
start_server leaf ""

# 4. The guest: UEFI, NVMe (snapshot), the serial console and QMP on unix sockets, the user network on USB.
. plan/tools/guest/qemu-accel.sh
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$out/vars.fd"
socket=$top/$out/serial.sock
qmp=$top/$out/qmp.sock
timeout 3000 qemu-system-x86_64 -name ws335-p2-p041 -machine q35 -m 4G -smp 4 $(qemu_accel_args max) \
    -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd" \
    -drive "if=pflash,format=raw,file=$out/vars.fd" \
    -drive "if=none,id=boot,file=$image,format=raw,snapshot=on" \
    -device nvme,serial=zedbsd-boot,drive=boot,bootindex=1 \
    -device qemu-xhci,id=xhci \
    -netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dhcpstart=10.0.2.15,dns=10.0.2.3 \
    -device usb-net,bus=xhci.0,port=1,id=ecm,netdev=net0,mac=52:54:00:33:50:41,msos-desc=on \
    -vga std -display none -no-reboot \
    -serial "unix:$socket,server=on,wait=off" -qmp "unix:$qmp,server=on,wait=off" -monitor none \
    > "$out/qemu.txt" 2>&1 &
qemu=$!
tries=0
while [ ! -S "$socket" ] && [ $tries -lt 600 ]; do
	kill -0 "$qemu" 2> /dev/null || fail "QEMU ended, $out/qemu.txt"
	sleep 0.1
	tries=$((tries + 1))
done
serial="$python plan/tools/guest/serial.py --socket $socket --timeout 300"
$serial expect 'login: ' > /dev/null || fail "no login prompt"
$serial login > /dev/null || fail "login"
echo "run-mailer-guest: logged in on the serial console"

# The network: a DHCP lease on ue0.
tries=0
while [ $tries -lt 15 ]; do
	$serial run 'dhcpc -t 20 ue0' > "$out/dhcpc.txt" 2>&1
	if $serial run 'ifconfig ue0' > "$out/ifconfig.txt" 2>&1 && grep -q 'inet 10\.0\.2\.' "$out/ifconfig.txt"; then
		break
	fi
	sleep 2
	tries=$((tries + 1))
done
grep -q 'inet 10\.0\.2\.' "$out/ifconfig.txt" || fail "no DHCP address on ue0, $out/ifconfig.txt"
echo "run-mailer-guest: ue0 $(grep -o 'inet 10\.0\.2\.[0-9]*' "$out/ifconfig.txt")"

# What the guest has: the compat libraries, and the names the image's mailer opens.
$serial run 'uname -a; ls -l /lib/libssl-compat.so /lib/libcrypto-compat.so /bin/mailer' > "$out/files.txt" 2>&1 ||
    { cat "$out/files.txt"; fail "the files in the guest"; }
tr -d '\r' < "$out/files.txt"
$serial run "strings /bin/mailer | grep '^lib.*\.so'" > "$out/strings.txt" 2>&1
tr -d '\r' < "$out/strings.txt" | grep -x 'lib\(ssl\|crypto\)\(-compat\)\{0,1\}\.so\(\.3\)\{0,1\}' > "$out/names.txt"
echo "run-mailer-guest: /bin/mailer opens: $(tr '\n' ' ' < "$out/names.txt")"
if ! grep -qx 'libssl-compat.so' "$out/names.txt" || grep -q '^libssl\.so' "$out/names.txt"; then
	echo "run-mailer-guest: /bin/mailer does not open libssl-compat.so alone"
	status=1
fi

# Sends a host file into the guest in base64 lines and checks it by cksum; send <file> <guest path>.
send() {
	base64 -w 300 "$1" > "$out/send.b64"
	$serial run ": > $2.b64" > /dev/null
	while IFS= read -r line; do
		$serial run "printf '%s\\n' '$line' >> $2.b64" > /dev/null || fail "writing $2"
	done < "$out/send.b64"
	$serial run "base64 -d $2.b64 > $2 && cksum $2" > "$out/send-cksum.txt" 2>&1
	[ "$(tr -d '\r' < "$out/send-cksum.txt" | grep "$2" | awk '{print $1, $2}')" = "$(cksum < "$1" | awk '{print $1, $2}')" ] ||
	    fail "$2 in the guest differs, $out/send-cksum.txt"
}
send "$out/mailer-tls" /root/mailer-tls
$serial run 'chmod 755 /root/mailer-tls' > /dev/null
send "$pki/ca.pem" /root/ca.pem
echo "run-mailer-guest: the test program and the CA are in the guest"

# 5. The cases: case <label> <arguments of mailer-tls>.
count=0
run_case() {
	label=$1
	shift
	count=$((count + 1))
	$serial run "MAILER_TLS_ADDRESS=10.0.2.2 /root/mailer-tls $*; echo exit=\$?" > "$out/case-$count.txt" 2>&1
	tr -d '\r' < "$out/case-$count.txt" > "$out/case-$count.clean.txt"
	result=$(grep '^mailer-tls: ' "$out/case-$count.clean.txt" | tail -1)
	echo "run-mailer-guest: $label: ${result#mailer-tls: }"
	case "$result" in
	"mailer-tls: PASS"*) grep -qx 'exit=0' "$out/case-$count.clean.txt" || status=1 ;;
	*) status=1; tail -5 "$out/case-$count.clean.txt" ;;
	esac
}
run_case "self-signed 1.3, no pin" - "$port_self" - untrusted "$fp_self"
run_case "self-signed 1.3, its pin" - "$port_self" "$fp_self" ok
run_case "self-signed 1.3, another pin" - "$port_self" "$fp_other" untrusted "$fp_self"
run_case "self-signed 1.2 RSA, its pin" - "$port_self12" "$fp_self12" ok
run_case "self-signed 1.2 RSA, another pin" - "$port_self12" "$fp_other" untrusted "$fp_self12"
run_case "CA-signed, the CA file" /root/ca.pem "$port_leaf" - ok
run_case "CA-signed, no CA file" - "$port_leaf" - untrusted "$fp_leaf"

$serial run 'sync' > /dev/null
sync
if [ $status -eq 0 ]; then
	echo "run-mailer-guest: PASS ($count cases)"
else
	echo "run-mailer-guest: FAIL"
fi
exit $status
