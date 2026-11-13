#!/bin/sh
# The host test of Mail's TLS (userland/desktop/mailer/tls.c), which calls libssl-compat and libcrypto-compat directly,
# against openssl s_server (-rev) on 127.0.0.1: mailer-tls.c is built with tls.c (ASan and UBSan) and with the two
# compat libraries built for the host as static archives (plan/tools/tls/host-compat-libs.sh).  The cases:
#   a self-signed server (TLS 1.3, P-256): without a pin ML_ERROR_UNTRUSTED with the certificate's SHA-256 fingerprint
#   as openssl x509 -fingerprint -sha256 gives it; with that pin the connection is made and a line goes each way; with
#   the pin of another certificate ML_ERROR_UNTRUSTED;
#   a self-signed server over TLS 1.2 (RSA): the same three;
#   a server whose certificate the tests' CA signed: with the CA file the connection is made without a pin, without
#   it ML_ERROR_UNTRUSTED with the leaf's fingerprint.
# The program has the compat libraries in itself: SSL_connect is defined in it, and it needs no libssl or libcrypto
# (readelf -d) and names no library to open (strings).
#   sh plan/ws335/tests/run-mailer-tls.sh
# Output under build/tmp/ws335-mailer-tls (a fresh run).  The last line is run-mailer-tls: PASS (<N> cases) or
# run-mailer-tls: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
PATH=/usr/bin:/bin:$PATH
export PATH
python=/usr/bin/python3
. plan/tools/fresh-out.sh
out=build/tmp/ws335-mailer-tls
fresh_out "$out"
status=0
servers=

# 1. The compat libraries for the host (static archives, shared by the runs: under build/tmp, not the fresh run's directory).
compat=build/tmp/ws335-mailer-tls-compat
if ! sh plan/tools/tls/host-compat-libs.sh "$compat" > "$out/compat-libs.log" 2>&1; then
	tail -20 "$out/compat-libs.log"
	echo "run-mailer-tls: FAIL (the compat libraries; $out/compat-libs.log)"
	exit 1
fi

# 2. The test program with tls.c and the two archives.
M=userland/desktop/mailer
if ! timeout 300 cc -std=gnu17 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I. -Iuserland/desktop/include \
    -I"$compat/include" plan/ws335/tests/mailer-tls.c $M/tls.c userland/desktop/libkeiland/platform/thread-posix.c \
    "$compat/libssl-compat.a" "$compat/libcrypto-compat.a" -lpthread \
    -o "$out/mailer-tls" > "$out/build.log" 2>&1 || [ -s "$out/build.log" ]; then
	head -20 "$out/build.log"
	echo "run-mailer-tls: FAIL (the build; $out/build.log)"
	exit 1
fi

# The libraries are in the program: SSL_connect defined once, no dynamic library of a TLS library, no name to open.
defined=$(nm "$out/mailer-tls" | grep -c ' T SSL_connect$')
needed=$(readelf -d "$out/mailer-tls" | grep -c 'NEEDED.*\(libssl\|libcrypto\)')
named=$(strings "$out/mailer-tls" | grep -c 'libssl\.so\|libssl-compat\.so\|libcrypto\.so\|libcrypto-compat\.so')
echo "mailer-tls: SSL_connect defined $defined times; NEEDED libssl/libcrypto $needed; names of a library to open $named"
if [ "$defined" -ne 1 ] || [ "$needed" -ne 0 ] || [ "$named" -ne 0 ]; then
	echo "mailer-tls: the program does not carry the compat libraries alone"
	status=1
fi

# 3. The certificates: two self-signed for localhost (P-256, RSA), one more for a wrong pin, and a CA with a leaf.
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
} > "$pki/openssl.log" 2>&1 || { tail -10 "$pki/openssl.log"; echo "run-mailer-tls: FAIL (certificates)"; exit 1; }

# The SHA-256 fingerprint of a certificate as Mail writes it (64 small hex digits), from openssl.
fingerprint() {
	openssl x509 -in "$1" -noout -fingerprint -sha256 | sed 's/^.*=//; s/://g' | tr 'A-F' 'a-f'
}
fp_self=$(fingerprint "$pki/self.pem")
fp_self12=$(fingerprint "$pki/self12.pem")
fp_other=$(fingerprint "$pki/other.pem")
fp_leaf=$(fingerprint "$pki/leaf.pem")

# 4. The servers: s_server -rev on ports the kernel chose just before.
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
start_server() {
	port=$(free_port)
	openssl s_server -rev -accept "127.0.0.1:$port" -cert "$pki/$1.pem" -key "$pki/$1.key" $2 \
	    > "$out/server-$1.log" 2>&1 < /dev/null &
	servers="$servers $!"
	wait_port "$port" || { echo "run-mailer-tls: FAIL (s_server $1 did not start)"; kill $servers; exit 1; }
	eval "port_$1=$port"
}
trap 'kill $servers 2> /dev/null' EXIT
start_server self ""
start_server self12 "-tls1_2"
start_server leaf ""

# 5. The cases.  run <label> <arguments...>
count=0
run() {
	label=$1
	shift
	count=$((count + 1))
	timeout 60 "$out/mailer-tls" "$@" > "$out/case-$count.txt" 2>&1
	code=$?
	result=$(grep '^mailer-tls: ' "$out/case-$count.txt" | tail -1)
	echo "mailer-tls: $label: ${result#mailer-tls: }"
	case "$result" in
	"mailer-tls: PASS"*) [ "$code" = 0 ] || { echo "  (exit $code)"; status=1; } ;;
	*) status=1; tail -5 "$out/case-$count.txt" ;;
	esac
}
run "self-signed 1.3, no pin" - "$port_self" - untrusted "$fp_self"
run "self-signed 1.3, its pin" - "$port_self" "$fp_self" ok
run "self-signed 1.3, another pin" - "$port_self" "$fp_other" untrusted "$fp_self"
run "self-signed 1.2 RSA, no pin" - "$port_self12" - untrusted "$fp_self12"
run "self-signed 1.2 RSA, its pin" - "$port_self12" "$fp_self12" ok
run "self-signed 1.2 RSA, another pin" - "$port_self12" "$fp_other" untrusted "$fp_self12"
run "CA-signed, the CA file" "$pki/ca.pem" "$port_leaf" - ok
run "CA-signed, no CA file" - "$port_leaf" - untrusted "$fp_leaf"

if [ $status -eq 0 ]; then
	echo "run-mailer-tls: PASS ($count cases)"
else
	echo "run-mailer-tls: FAIL"
fi
exit $status
