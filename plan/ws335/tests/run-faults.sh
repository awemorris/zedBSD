#!/bin/sh
# Failure paths of libssl-compat through its public interface, against servers on 127.0.0.1:
#   1. Allocation failures: tls-client.c is built for the host with malloc-fault.c (the libraries' malloc, calloc and
#      realloc wrapped, ASan and UBSan as run-interop.sh builds it) and connects to the host's "openssl s_server
#      -rev" (TLS 1.3 and TLS 1.2, a P-256 chain).  A first run counts the allocations of a whole connection; then
#      each allocation in turn is made to fail, once per run.  Every run must end by itself (no sanitizer report,
#      no crash, no hang): either the connection still completes, or the client fails.  At least one run must
#      report the allocation failure (ERR_R_MALLOC_FAILURE, reason 786688, "out of memory") through ERR_get_error.
#   2. missing_extension: tls-raw-server.py answers a ClientHello with a TLS 1.3 ServerHello without key_share; the
#      client must send the fatal alert 109 (RFC 8446 4.2.8) and queue SSL_R_BAD_EXTENSION (110).
#   sh plan/ws335/tests/run-faults.sh
# Each server listens on 127.0.0.1 only, on a port the kernel chose, and is checked (ss) to be this script's own
# process.  The last line is run-faults: PASS or run-faults: FAIL.  Output under build/tmp/ws335-faults (a fresh run).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
PATH=/usr/bin:/bin:$PATH
export PATH
python=/usr/bin/python3
openssl=${OPENSSL:-openssl}
. plan/tools/fresh-out.sh
out=build/tmp/ws335-faults
fresh_out "$out"
status=0

# The client with the allocation wrapper.
mkdir -p "$out/include"
ln -sfn "$top/include/libc/compat" "$out/include/compat"
modules=
for file in userland/base/libcrypto-compat/*.c userland/base/libssl-compat/*.c; do
	case "$file" in
	*-win32.c) ;;
	*) modules="$modules $file" ;;
	esac
done
client=$out/tls-client-fault
if ! timeout 600 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
    -fno-sanitize-recover=all -fno-omit-frame-pointer -I"$out/include" -I. plan/ws335/tests/tls-client.c \
    plan/ws335/tests/malloc-fault.c $modules -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc -lpthread \
    -o "$client" > "$out/build.log" 2>&1 || [ -s "$out/build.log" ]; then
	head -20 "$out/build.log"
	echo "run-faults: FAIL (client build; $out/build.log)"
	exit 1
fi

# A P-256 root and a leaf for interop.test.
pki=$out/pki
mkdir -p "$pki"
cat > "$pki/openssl.cnf" << 'EOF'
[req]
distinguished_name = dn
prompt = no
[dn]
CN = unused
[ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
[leaf]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:interop.test
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
EOF
{
	"$openssl" genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$pki/root.key" &&
	"$openssl" req -x509 -new -key "$pki/root.key" -subj "/CN=ws335 faults root" -config "$pki/openssl.cnf" \
	    -extensions ca -days 30 -sha256 -out "$pki/root.pem" &&
	"$openssl" genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$pki/leaf.key" &&
	"$openssl" req -new -key "$pki/leaf.key" -subj "/CN=interop.test" -config "$pki/openssl.cnf" -out "$pki/leaf.csr" &&
	"$openssl" x509 -req -in "$pki/leaf.csr" -CA "$pki/root.pem" -CAkey "$pki/root.key" -set_serial 2 -days 30 \
	    -sha256 -extfile "$pki/openssl.cnf" -extensions leaf -out "$pki/leaf.pem"
} > "$pki/pki.log" 2>&1 || { echo "run-faults: FAIL (test PKI; $pki/pki.log)"; exit 1; }

free_port() {
	"$python" -I -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])'
}

# Waits until the process started by timeout (pid $1) listens on 127.0.0.1:$2; returns 1 if it does not.
own_listener() {
	tries=0
	while [ $tries -lt 100 ]; do
		for pid in $(ss -ltnpH "src 127.0.0.1:$2" 2> /dev/null | sed -n 's/.*pid=\([0-9]*\),.*/\1/p'); do
			[ "$(ps -o ppid= -p "$pid" | tr -d ' ')" = "$1" ] && return 0
		done
		kill -0 "$1" 2> /dev/null || return 1
		tries=$((tries + 1))
		sleep 0.1
	done
	return 1
}

# One connection to a fresh s_server: version flag ($1), then the environment of the client ($2); output to $3.
connect_once() {
	port=$(free_port)
	timeout 60 "$openssl" s_server -accept "127.0.0.1:$port" -naccept 1 -rev "$1" -cert "$pki/leaf.pem" \
	    -key "$pki/leaf.key" < /dev/null > "$3.server" 2>&1 &
	server=$!
	if ! own_listener "$server" "$port"; then
		kill "$server" 2> /dev/null
		wait "$server" 2> /dev/null
		echo "the server did not listen" > "$3"
		return 2
	fi
	env $2 ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 timeout 60 "$client" \
	    -a 127.0.0.1 -s 200 interop.test "$port" "$pki/root.pem" > "$3" 2>&1
	code=$?
	tries=0
	while [ $tries -lt 50 ] && kill -0 "$server" 2> /dev/null; do
		tries=$((tries + 1))
		sleep 0.1
	done
	kill "$server" 2> /dev/null
	wait "$server" 2> /dev/null
	return $code
}

# 1. Every allocation of a TLS 1.3 and a TLS 1.2 connection, failed one at a time.
for version in 1.3 1.2; do
	flag=-tls1_3
	[ $version = 1.2 ] && flag=-tls1_2
	dir=$out/alloc-$version
	mkdir -p "$dir"
	connect_once $flag WS335_COUNT_ALLOC=1 "$dir/count.txt"
	total=$(sed -n 's/^malloc-fault: \([0-9]*\) allocations$/\1/p' "$dir/count.txt" | tail -1)
	if [ "$(grep -c '^tls-client: PASS' "$dir/count.txt")" -ne 1 ] || [ -z "$total" ] || [ "$total" -lt 1 ]; then
		echo "faults: $version: the counting run did not pass ($dir/count.txt)"
		status=1
		continue
	fi
	completed=0
	refused=0
	reported=0
	bad=0
	n=1
	while [ $n -le "$total" ]; do
		connect_once $flag WS335_FAIL_ALLOC=$n "$dir/fail-$n.txt"
		code=$?
		last=$(tail -1 "$dir/fail-$n.txt")
		if grep -q -E 'Sanitizer|runtime error' "$dir/fail-$n.txt" || { [ $code -ne 0 ] && [ $code -ne 1 ]; }; then
			bad=$((bad + 1))
			echo "faults: $version: allocation $n: exit $code, $last ($dir/fail-$n.txt)"
		elif [ "$last" = "tls-client: PASS" ]; then
			completed=$((completed + 1))
		else
			refused=$((refused + 1))
			grep -q 'reason 786688:' "$dir/fail-$n.txt" && reported=$((reported + 1))
		fi
		n=$((n + 1))
	done
	echo "faults: TLS $version: $total allocations; failing each: $completed connections still completed, $refused failed with an error ($reported reported malloc failure), $bad ended badly"
	if [ $bad -ne 0 ] || [ $reported -eq 0 ]; then
		status=1
	fi
done

# 2. A TLS 1.3 ServerHello without key_share: alert 109 and SSL_R_BAD_EXTENSION.
dir=$out/missing-extension
mkdir -p "$dir"
timeout 40 "$python" -I plan/ws335/tests/tls-raw-server.py no-key-share "$dir/port" > "$dir/server.txt" 2>&1 &
server=$!
tries=0
while [ ! -s "$dir/port" ] && [ $tries -lt 100 ]; do
	sleep 0.1
	tries=$((tries + 1))
done
port=$(cat "$dir/port" 2> /dev/null)
if [ -z "$port" ] || ! own_listener "$server" "$port"; then
	echo "faults: missing_extension: the raw server did not listen"
	status=1
else
	ASAN_OPTIONS=detect_leaks=1 timeout 60 "$client" -a 127.0.0.1 -x reason=110 interop.test "$port" \
	    "$pki/root.pem" > "$dir/client.txt" 2>&1
	wait "$server"
	if [ "$(tail -1 "$dir/client.txt")" = "tls-client: PASS" ] && grep -q '^alert 2 109$' "$dir/server.txt"; then
		echo "faults: missing_extension: the client sent the fatal alert 109 and queued reason 110: PASS"
	else
		echo "faults: missing_extension: FAIL (client: $(tail -1 "$dir/client.txt"); server: $(tail -1 "$dir/server.txt"))"
		status=1
	fi
fi

if [ $status -eq 0 ]; then
	echo "run-faults: PASS"
else
	echo "run-faults: FAIL"
fi
exit $status
