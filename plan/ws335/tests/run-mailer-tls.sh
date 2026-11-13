#!/bin/sh
# The host test of Mail's TLS (userland/desktop/mailer/tls.c) with libssl-compat in place of OpenSSL, against
# openssl s_server (-rev) on 127.0.0.1: mailer-tls.c is built twice with tls.c (ASan and UBSan), as it is (the host's
# OpenSSL) and with -DML_TLS_COMPAT (it opens libcrypto-compat.so and libssl-compat.so only); the compat libraries are
# built for the host as shared libraries under their own names, and the compat runs get LD_LIBRARY_PATH to them.
# The same cases run with both builds:
#   a self-signed server (TLS 1.3, P-256): without a pin ML_ERROR_UNTRUSTED with the certificate's SHA-256 fingerprint
#   as openssl x509 -fingerprint -sha256 gives it; with that pin the connection is made and a line goes each way; with
#   the pin of another certificate ML_ERROR_UNTRUSTED;
#   a self-signed server over TLS 1.2 (RSA): the same three;
#   a server whose certificate the tests' CA signed: with the CA file the connection is made without a pin, without
#   it ML_ERROR_UNTRUSTED with the leaf's fingerprint.
# The compat build without LD_LIBRARY_PATH fails with EPROTONOSUPPORT naming libssl-compat.so (no fall back to
# OpenSSL), and the dynamic loader's log shows libssl-compat.so loaded from the compat directory in the compat runs
# and never in the OpenSSL runs.
#   sh plan/ws335/tests/run-mailer-tls.sh
# Output under build/tmp/ws335-mailer-tls (a fresh run).  The last line is run-mailer-tls: PASS or run-mailer-tls: FAIL.
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

# 1. The two builds of the test program.
M=userland/desktop/mailer
for flavour in openssl compat; do
	define=
	[ "$flavour" = compat ] && define=-DML_TLS_COMPAT
	if ! timeout 300 cc -std=gnu17 -D_GNU_SOURCE $define -O1 -g -Wall -Wextra -Werror -pthread \
	    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I. -Iuserland/desktop/include \
	    plan/ws335/tests/mailer-tls.c $M/tls.c userland/desktop/libkeiland/platform/thread-posix.c \
	    -ldl -o "$out/mailer-tls-$flavour" > "$out/build-$flavour.log" 2>&1 || [ -s "$out/build-$flavour.log" ]; then
		head -20 "$out/build-$flavour.log"
		echo "run-mailer-tls: FAIL (the $flavour build; $out/build-$flavour.log)"
		exit 1
	fi
done
if ! strings "$out/mailer-tls-compat" | grep -q '^libssl-compat\.so$' || strings "$out/mailer-tls-compat" | grep -q '^libssl\.so'; then
	echo "mailer-tls: the compat build does not name libssl-compat.so alone"
	status=1
fi
if strings "$out/mailer-tls-openssl" | grep -q '^libssl-compat\.so$'; then
	echo "mailer-tls: the OpenSSL build names libssl-compat.so"
	status=1
fi

# 2. The compat libraries as host shared libraries (as run-nettls.sh builds them).
lib=$out/lib
mkdir -p "$lib/include" "$lib/obj"
ln -sfn "$top/include/libc/compat" "$lib/include/compat"
build_library() {
	name=$1
	shift
	objects=
	for file in "$@"; do
		object=$lib/obj/$name-$(basename "$file" .c).o
		if ! timeout 300 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fPIC -I"$lib/include" -I. \
		    -c "$file" -o "$object" >> "$lib/build.log" 2>&1; then
			return 1
		fi
		objects="$objects $object"
	done
	cc -shared -Wl,-soname,$name.so -Wl,-z,defs -Wl,-z,nodelete -Wl,--version-script=userland/base/$name/exports.map \
	    $objects -L"$lib" $libraries -lpthread -o "$lib/$name.so" >> "$lib/build.log" 2>&1
}
crypto_sources=$(grep -v '^#' userland/base/libcrypto-compat/sources.mk | grep -o 'userland/base/libcrypto-compat/[a-z0-9-]*\.c' | grep -v -- '-win32\.c')
ssl_sources=$(grep -v '^#' userland/base/libssl-compat/sources.mk | grep -o 'userland/base/libssl-compat/[a-z0-9-]*\.c')
: > "$lib/build.log"
libraries=
build_library libcrypto-compat $crypto_sources || { tail -20 "$lib/build.log"; echo "run-mailer-tls: FAIL (libcrypto-compat.so)"; exit 1; }
libraries=-l:libcrypto-compat.so
build_library libssl-compat $ssl_sources || { tail -20 "$lib/build.log"; echo "run-mailer-tls: FAIL (libssl-compat.so)"; exit 1; }
if [ -s "$lib/build.log" ]; then
	head -20 "$lib/build.log"
	echo "run-mailer-tls: FAIL (warnings in the library build; $lib/build.log)"
	exit 1
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

# 5. The cases, with each build.  run <flavour> <label> <arguments...>
count=0
run() {
	flavour=$1
	label=$2
	shift 2
	count=$((count + 1))
	if [ "$flavour" = compat ]; then
		LD_LIBRARY_PATH=$top/$lib LD_DEBUG=libs LD_DEBUG_OUTPUT=$top/$out/ld-$flavour-$count \
		    timeout 60 "$out/mailer-tls-$flavour" "$@" > "$out/case-$count.txt" 2>&1
	else
		LD_DEBUG=libs LD_DEBUG_OUTPUT=$top/$out/ld-$flavour-$count \
		    timeout 60 "$out/mailer-tls-$flavour" "$@" > "$out/case-$count.txt" 2>&1
	fi
	code=$?
	result=$(grep '^mailer-tls: ' "$out/case-$count.txt" | tail -1)
	echo "mailer-tls: $flavour: $label: ${result#mailer-tls: }"
	case "$result" in
	"mailer-tls: PASS"*) [ "$code" = 0 ] || { echo "  (exit $code)"; status=1; } ;;
	*) status=1; tail -5 "$out/case-$count.txt" ;;
	esac
}
for flavour in openssl compat; do
	run $flavour "self-signed 1.3, no pin" - "$port_self" - untrusted "$fp_self"
	run $flavour "self-signed 1.3, its pin" - "$port_self" "$fp_self" ok
	run $flavour "self-signed 1.3, another pin" - "$port_self" "$fp_other" untrusted "$fp_self"
	run $flavour "self-signed 1.2 RSA, no pin" - "$port_self12" - untrusted "$fp_self12"
	run $flavour "self-signed 1.2 RSA, its pin" - "$port_self12" "$fp_self12" ok
	run $flavour "self-signed 1.2 RSA, another pin" - "$port_self12" "$fp_other" untrusted "$fp_self12"
	run $flavour "CA-signed, the CA file" "$pki/ca.pem" "$port_leaf" - ok
	run $flavour "CA-signed, no CA file" - "$port_leaf" - untrusted "$fp_leaf"
done

# 6. The compat build without its libraries: EPROTONOSUPPORT naming them, not OpenSSL.
count=$((count + 1))
timeout 60 "$out/mailer-tls-compat" - "$port_self" - noload libssl-compat.so > "$out/case-$count.txt" 2>&1
code=$?
result=$(grep '^mailer-tls: ' "$out/case-$count.txt" | tail -1)
echo "mailer-tls: compat: without the libraries: ${result#mailer-tls: }"
case "$result" in
"mailer-tls: PASS"*) [ "$code" = 0 ] || status=1 ;;
*) status=1 ;;
esac

# 7. The libraries loaded: libssl-compat from the compat directory in every compat run, never in an OpenSSL run;
# OpenSSL's libssl in the OpenSSL runs and never in a compat run.
compat_loads=$(cat "$out"/ld-compat-* 2> /dev/null | grep -c "calling init: $top/$lib/libssl-compat\.so")
compat_openssl=$(cat "$out"/ld-compat-* 2> /dev/null | grep -c 'calling init: .*/libssl\.so')
openssl_loads=$(cat "$out"/ld-openssl-* 2> /dev/null | grep -c 'calling init: .*/libssl\.so')
openssl_compat=$(cat "$out"/ld-openssl-* 2> /dev/null | grep -c 'libssl-compat')
echo "mailer-tls: loader logs: compat runs init libssl-compat $compat_loads times and OpenSSL's libssl $compat_openssl times; OpenSSL runs init libssl $openssl_loads times and libssl-compat $openssl_compat times"
if [ "$compat_loads" -ne 8 ] || [ "$compat_openssl" -ne 0 ] || [ "$openssl_loads" -ne 8 ] || [ "$openssl_compat" -ne 0 ]; then
	echo "mailer-tls: the libraries loaded are not the ones meant"
	status=1
fi

if [ $status -eq 0 ]; then
	echo "run-mailer-tls: PASS ($count cases)"
else
	echo "run-mailer-tls: FAIL"
fi
exit $status
