#!/bin/sh
# The interop test of libssl-compat against the host's OpenSSL server: tls-client.c (the public interface only) is
# built and connects to "openssl s_server -rev" (each line comes back reversed) over 127.0.0.1, for each version,
# suite, group, key and signature scheme below, with fragmented records, a non-blocking socket and the server's
# close with and without close_notify.  The server's own report (-trace into a file of its own) says what was
# negotiated, and each case checks it: the version, the suite, the group (with the HelloRetryRequest for P-256) and
# the signature scheme.  A few cases must fail in a set way (an unknown CA, another name, an RSA key below 2048
# bits).
#   sh plan/ws335/tests/run-interop.sh            the host: tls-client built with the host's cc from the sources
#                                                 of both libraries (ASan and UBSan), as run-unit.sh builds them
#   sh plan/ws335/tests/run-interop.sh --win32    Windows: the two static libraries and tls-client.exe built with
#                                                 llvm-mingw as run-unit.sh --win32 builds them (x86_64, and
#                                                 aarch64 to the link only), the x86_64 .exe run under Wine
#                                                 (prefix build/tmp/ws335-wine) against the same servers: a subset
#                                                 of the cases, then the trust sources of Windows: cert.pem beside
#                                                 the .exe with SSL_CTX_set_default_verify_paths, and the test
#                                                 intermediate in the current user's Disallowed store
#   sh plan/ws335/tests/run-interop.sh --client PATH   the cases with a client built elsewhere (it is run as
#                                                 given; the guest test runs its own client)
# Each case prints one line "interop: <name>: PASS" or "interop: <name>: FAIL (<why>; <dir>)"; the last line is
# "run-interop: PASS (<n> cases)" or "run-interop: FAIL (<failed> of <n> cases)".  Output under build/tmp/ws335-interop
# (a fresh run directory); each case's client and server output is in its own directory.
# Every server listens on 127.0.0.1 only, on a port the kernel chose, and is checked (ss) to be this script's own
# process before the client connects; each runs at most 90 seconds and ends after one connection.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
PATH=/usr/bin:/bin:$PATH
export PATH
. plan/tools/fresh-out.sh
out=build/tmp/ws335-interop
fresh_out "$out"
openssl=${OPENSSL:-openssl}
python=/usr/bin/python3

system=posix
client=
case "${1:-}" in
--win32) system=win32; shift ;;
--client) system=given; client=$2; shift 2 ;;
esac
if [ $# -ne 0 ]; then
	echo "usage: sh plan/ws335/tests/run-interop.sh [--win32 | --client PATH]"
	exit 2
fi
if ! command -v "$openssl" > /dev/null 2>&1 || ! command -v ss > /dev/null 2>&1; then
	echo "run-interop: FAIL (needs $openssl and ss)"
	exit 1
fi
echo "interop: server $("$openssl" version)"

# The modules of both libraries, without the files of the other system.
modules=
for file in userland/base/libcrypto-compat/*.c userland/base/libssl-compat/*.c; do
	case "$file" in
	*-win32.c) ;;
	*) modules="$modules $file" ;;
	esac
done
mkdir -p "$out/include"
ln -sfn "$top/include/libc/compat" "$out/include/compat"

# 1. The client.
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
wine_prefix=$top/build/tmp/ws335-wine
wflags="-std=c89 -O1 -g -Wall -Wextra -Werror -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE
	-fsanitize=undefined -fsanitize-trap=undefined -I$out/include -I."
if [ "$system" = posix ]; then
	client=$out/tls-client
	if ! timeout 600 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
	    -fno-sanitize-recover=all -fno-omit-frame-pointer -I"$out/include" -I. plan/ws335/tests/tls-client.c $modules \
	    -lpthread -o "$client" > "$out/build.log" 2>&1 || [ -s "$out/build.log" ]; then
		head -20 "$out/build.log"
		echo "run-interop: FAIL (client build; $out/build.log)"
		exit 1
	fi
elif [ "$system" = win32 ]; then
	if ! command -v wine > /dev/null 2>&1; then
		echo "run-interop: FAIL (--win32 needs wine)"
		exit 1
	fi
	# The two static libraries and the programs for each architecture (warnings are errors; aarch64 only links).
	for arch in x86_64 aarch64; do
		wcc=$LLVM_MINGW/bin/$arch-w64-mingw32-clang
		lib=$out/$arch
		mkdir -p "$lib"
		if [ ! -x "$wcc" ]; then
			echo "run-interop: FAIL (no $wcc)"
			exit 1
		fi
		for file in userland/base/libcrypto-compat/*.c userland/base/libssl-compat/*.c; do
			case "$file" in
			*-posix.c) continue ;;
			userland/base/libcrypto-compat/*) object=$lib/crypto-$(basename "$file" .c).o ;;
			*) object=$lib/ssl-$(basename "$file" .c).o ;;
			esac
			if ! timeout 300 $wcc $wflags -c "$file" -o "$object" > "$lib/build.log" 2>&1 || [ -s "$lib/build.log" ]; then
				head -20 "$lib/build.log"
				echo "run-interop: FAIL ($arch build of $file)"
				exit 1
			fi
		done
		"$LLVM_MINGW/bin/llvm-ar" rcs "$lib/libcrypto-compat.a" "$lib"/crypto-*.o
		"$LLVM_MINGW/bin/llvm-ar" rcs "$lib/libssl-compat.a" "$lib"/ssl-*.o
		for program in tls-client win-store; do
			if ! timeout 300 $wcc $wflags plan/ws335/tests/$program.c -L"$lib" -lssl-compat -lcrypto-compat \
			    -lbcrypt -lcrypt32 -lws2_32 -o "$lib/$program.exe" > "$lib/$program.log" 2>&1 || [ -s "$lib/$program.log" ]; then
				head -20 "$lib/$program.log"
				echo "run-interop: FAIL ($arch link of $program.exe)"
				exit 1
			fi
		done
		echo "interop: $arch libcrypto-compat.a, libssl-compat.a, tls-client.exe and win-store.exe built, no warning"
	done
	client=$out/x86_64/tls-client.exe
	if [ ! -f "$wine_prefix/.ws335-ready" ]; then
		mkdir -p "$wine_prefix"
		if WINEPREFIX=$wine_prefix WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=' timeout 600 wineboot -i \
		    > "$out/wineboot.log" 2>&1; then
			: > "$wine_prefix/.ws335-ready"
		else
			echo "run-interop: FAIL (wineboot; $out/wineboot.log)"
			exit 1
		fi
	fi
fi

# 2. The test PKI: for each key a root, an intermediate and a leaf for interop.test, 127.0.0.1 and 10.0.2.2 (the
# host as a QEMU guest sees it); RSA 4096's certificates are signed with RSASSA-PSS.  Also a root no client trusts
# with its own leaf, and an RSA 1024 leaf.
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
authorityKeyIdentifier = keyid:always
[leaf]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:interop.test,IP:127.0.0.1,IP:10.0.2.2
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
EOF
pki_fail() {
	echo "run-interop: FAIL (test PKI: $1; $pki/pki.log)"
	exit 1
}
make_key() {
	case "$2" in
	p256) "$openssl" genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$1" ;;
	p384) "$openssl" genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out "$1" ;;
	rsa*) "$openssl" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:"${2#rsa}" -out "$1" ;;
	esac >> "$pki/pki.log" 2>&1
}
# The digest and padding options of the certificates each issuer key signs.
sign_options() {
	case "$1" in
	p256|rsa2048|rsa1024) echo "-sha256" ;;
	p384) echo "-sha384" ;;
	rsa4096) echo "-sha384 -sigopt rsa_padding_mode:pss -sigopt rsa_pss_saltlen:digest" ;;
	esac
}
# Valid from a day before now (a guest's clock may lag) for 30 days.
not_before=$(date -u -d '1 day ago' +%Y%m%d%H%M%SZ)
for kind in p256 p384 rsa2048 rsa4096 other; do
	key=$kind
	[ "$kind" = other ] && key=p256
	make_key "$pki/root-$kind.key" $key || pki_fail "root key $kind"
	"$openssl" req -x509 -new -key "$pki/root-$kind.key" -subj "/CN=ws335 interop root $kind" -config "$pki/openssl.cnf" \
	    -extensions ca -not_before "$not_before" -days 30 $(sign_options $key) -out "$pki/root-$kind.pem" >> "$pki/pki.log" 2>&1 ||
	    pki_fail "root $kind"
	issuer=root-$kind
	if [ "$kind" != other ]; then
		make_key "$pki/int-$kind.key" $key || pki_fail "intermediate key $kind"
		"$openssl" req -new -key "$pki/int-$kind.key" -subj "/CN=ws335 interop intermediate $kind" \
		    -config "$pki/openssl.cnf" -out "$pki/int-$kind.csr" >> "$pki/pki.log" 2>&1 || pki_fail "intermediate request $kind"
		"$openssl" x509 -req -in "$pki/int-$kind.csr" -CA "$pki/root-$kind.pem" -CAkey "$pki/root-$kind.key" -set_serial 2 \
		    -not_before "$not_before" -days 30 $(sign_options $key) -extfile "$pki/openssl.cnf" -extensions ca \
		    -out "$pki/int-$kind.pem" >> "$pki/pki.log" 2>&1 || pki_fail "intermediate $kind"
		issuer=int-$kind
	fi
	make_key "$pki/leaf-$kind.key" $key || pki_fail "leaf key $kind"
	"$openssl" req -new -key "$pki/leaf-$kind.key" -subj "/CN=interop.test" -config "$pki/openssl.cnf" \
	    -out "$pki/leaf-$kind.csr" >> "$pki/pki.log" 2>&1 || pki_fail "leaf request $kind"
	"$openssl" x509 -req -in "$pki/leaf-$kind.csr" -CA "$pki/$issuer.pem" -CAkey "$pki/$issuer.key" -set_serial 3 \
	    -not_before "$not_before" -days 30 $(sign_options $key) -extfile "$pki/openssl.cnf" -extensions leaf \
	    -out "$pki/leaf-$kind.pem" >> "$pki/pki.log" 2>&1 || pki_fail "leaf $kind"
done
make_key "$pki/leaf-rsa1024.key" rsa1024 || pki_fail "leaf key rsa1024"
"$openssl" req -new -key "$pki/leaf-rsa1024.key" -subj "/CN=interop.test" -config "$pki/openssl.cnf" \
    -out "$pki/leaf-rsa1024.csr" >> "$pki/pki.log" 2>&1 || pki_fail "leaf request rsa1024"
"$openssl" x509 -req -in "$pki/leaf-rsa1024.csr" -CA "$pki/int-rsa2048.pem" -CAkey "$pki/int-rsa2048.key" -set_serial 4 \
    -not_before "$not_before" -days 30 -sha256 -extfile "$pki/openssl.cnf" -extensions leaf \
    -out "$pki/leaf-rsa1024.pem" >> "$pki/pki.log" 2>&1 || pki_fail "leaf rsa1024"
cp "$pki/int-rsa2048.pem" "$pki/int-rsa1024.pem"
cp "$pki/int-rsa2048.key" "$pki/int-rsa1024.key"
: > "$pki/int-other.pem"
cat "$pki/root-p256.pem" "$pki/root-p384.pem" "$pki/root-rsa2048.pem" "$pki/root-rsa4096.pem" > "$pki/ca.pem"
"$openssl" x509 -in "$pki/int-p256.pem" -outform DER -out "$pki/int-p256.der" 2>> "$pki/pki.log" || pki_fail "DER"
echo "interop: test PKI made (p256, p384, rsa2048, rsa4096 with RSASSA-PSS, an untrusted root, an rsa1024 leaf)"

# 3. The cases.
cases=0
failed=0
limits=0
number=0

# Prints a free port of 127.0.0.1 (the kernel's choice).
free_port() {
	"$python" -I -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])'
}

# Starts s_server for key with the extra arguments; sets server_pid, port and dir.  Fails unless the process that
# listens on 127.0.0.1:port is this one.
start_server() {
	key=$1
	shift
	port=$(free_port)
	set -- -accept "127.0.0.1:$port" -naccept 1 -rev -cert "$pki/leaf-$key.pem" -key "$pki/leaf-$key.key" "$@"
	if [ -s "$pki/int-$key.pem" ]; then
		set -- "$@" -cert_chain "$pki/int-$key.pem"
	fi
	timeout 90 "$openssl" s_server "$@" -trace -msgfile "$dir/trace.txt" < /dev/null > "$dir/server.txt" 2>&1 &
	server_pid=$!
	listen_pid=
	tries=0
	while [ $tries -lt 100 ]; do
		# The listener must be the s_server that timeout (server_pid) started.
		for pid in $(ss -ltnpH "src 127.0.0.1:$port" 2> /dev/null | sed -n 's/.*pid=\([0-9]*\),.*/\1/p'); do
			if [ "$(ps -o ppid= -p "$pid" | tr -d ' ')" = "$server_pid" ]; then
				listen_pid=$pid
				return 0
			fi
		done
		if ! kill -0 "$server_pid" 2> /dev/null; then
			break
		fi
		tries=$((tries + 1))
		sleep 0.1
	done
	kill "$server_pid" 2> /dev/null
	wait "$server_pid" 2> /dev/null
	return 1
}

# Runs the client (its arguments) for the case's directory; the output goes to dir/client.txt.
run_client() {
	if [ "$system" = win32 ]; then
		(cd "$dir" && LC_ALL=C.UTF-8 WINEPREFIX=$wine_prefix WINEDEBUG=-all WINEDLLOVERRIDES=winedbg.exe=d \
		    timeout 120 wine "$client_exe" "$@") > "$dir/client.txt" 2>&1
	else
		timeout 120 "$client" "$@" > "$dir/client.txt" 2>&1
	fi
}

# The CA file as the client names it.
ca_path() {
	if [ "$system" = win32 ]; then
		echo "Z:$top/$pki/$1"
	else
		echo "$top/$pki/$1"
	fi
}

# One case: name, key, the server's arguments (one word, split at spaces), the client's arguments (likewise), then
# what the server's trace and report must hold: words "v13", "v12", "hrr", "group=<Peer Temp Key text>",
# "suite=<name>", "scheme=<name>", "fail" (the server must not finish a handshake).
interop() {
	name=$1
	key=$2
	server_args=$3
	client_args=$4
	shift 4
	number=$((number + 1))
	cases=$((cases + 1))
	dir=$out/case-$number
	mkdir -p "$dir"
	echo "$name" > "$dir/name.txt"
	client_exe=$client
	if [ "$system" = win32 ]; then
		client_exe=$dir/tls-client.exe
		cp "$client" "$client_exe"
		# cert.pem beside the .exe (SSL_CTX_set_default_verify_paths reads it).
		if [ "${case_cert_pem:-}" = yes ]; then
			cp "$pki/ca.pem" "$dir/cert.pem"
		fi
	fi
	ca_arg=$(ca_path ca.pem)
	[ "${case_ca:-}" = none ] && ca_arg=
	why=
	# shellcheck disable=SC2086
	if ! start_server "$key" $server_args; then
		why="the server did not listen"
	else
		case " $client_args " in
		*" -c eof"*)
			# The server vanishes without a close_notify once the client has its line back.
			# shellcheck disable=SC2086
			run_client -a 127.0.0.1 $client_args "${case_host:-interop.test}" "$port" $ca_arg &
			client_pid=$!
			tries=0
			while [ $tries -lt 300 ] && ! grep -q 'waiting for eof' "$dir/client.txt" 2> /dev/null; do
				kill -0 "$client_pid" 2> /dev/null || break
				tries=$((tries + 1))
				sleep 0.1
			done
			kill -KILL "$listen_pid" 2> /dev/null
			wait "$client_pid"
			;;
		*)
			# shellcheck disable=SC2086
			run_client -a 127.0.0.1 $client_args "${case_host:-interop.test}" "$port" $ca_arg
			;;
		esac
		wait "$server_pid" 2> /dev/null
		last=$(tr -d '\r' < "$dir/client.txt" | tail -1)
		[ "$last" = "tls-client: PASS" ] || why="client: $last"
	fi

	# What the server negotiated.
	for word in "$@"; do
		[ -n "$why" ] && break
		case "$word" in
		v13) grep -q '^Protocol version: TLSv1.3' "$dir/server.txt" || why="not TLS 1.3" ;;
		v12) grep -q '^Protocol version: TLSv1.2' "$dir/server.txt" || why="not TLS 1.2" ;;
		suite=*) grep -q "^Ciphersuite: ${word#suite=}\$" "$dir/server.txt" || why="not the suite ${word#suite=}" ;;
		group=*)
			# TLS 1.3: the server's report; TLS 1.2: the curve of its ServerKeyExchange.
			if grep -q '^Protocol version: TLSv1.2' "$dir/server.txt"; then
				grep -q "named_curve: .*(${word#group=})\$" "$dir/trace.txt" || why="not the group ${word#group=}"
			else
				grep -q "^Peer Temp Key: $(group_text ${word#group=})" "$dir/server.txt" || why="not the group ${word#group=}"
			fi ;;
		hrr) grep -q 'gmt_unix_time=0xCF21AD74' "$dir/trace.txt" || why="no HelloRetryRequest" ;;
		nohrr) ! grep -q 'gmt_unix_time=0xCF21AD74' "$dir/trace.txt" || why="a HelloRetryRequest" ;;
		scheme=*) grep -q "Signature Algorithm: ${word#scheme=} (0x" "$dir/trace.txt" || why="not the scheme ${word#scheme=}" ;;
		fail) ! grep -q '^CONNECTION ESTABLISHED' "$dir/server.txt" || why="the server finished a handshake" ;;
		alert=*) grep -q "Level=fatal(2), description=.*(${word#alert=})\$" "$dir/trace.txt" || why="no fatal alert ${word#alert=}" ;;
		no-shared-cipher) grep -q ':no shared cipher:' "$dir/server.txt" || why="the server did not refuse for no shared cipher" ;;
		esac
	done
	if [ -z "$why" ] && [ "${case_limit:-}" = yes ]; then
		limits=$((limits + 1))
		echo "interop: $name: PASS (known limit)"
	elif [ -z "$why" ]; then
		echo "interop: $name: PASS"
	else
		failed=$((failed + 1))
		echo "interop: $name: FAIL ($why; $dir)"
	fi
}

suites13="TLS_AES_128_GCM_SHA256 TLS_AES_256_GCM_SHA384 TLS_CHACHA20_POLY1305_SHA256"
scheme_of() {
	case "$1" in
	p256) echo ecdsa_secp256r1_sha256 ;;
	p384) echo ecdsa_secp384r1_sha384 ;;
	rsa*) echo rsa_pss_rsae_sha256 ;;
	esac
}
suites12_of() {
	case "$1" in
	p*) echo "ECDHE-ECDSA-AES128-GCM-SHA256 ECDHE-ECDSA-AES256-GCM-SHA384 ECDHE-ECDSA-CHACHA20-POLY1305" ;;
	rsa*) echo "ECDHE-RSA-AES128-GCM-SHA256 ECDHE-RSA-AES256-GCM-SHA384 ECDHE-RSA-CHACHA20-POLY1305" ;;
	esac
}
# The server's report of a TLS 1.3 group by its code point.
group_text() {
	case "$1" in
	29) echo "X25519" ;;
	23) echo "ECDH, prime256v1" ;;
	esac
}

if [ "$system" = win32 ]; then
	keys="p256 rsa2048"
	suites13_run="TLS_AES_256_GCM_SHA384"
	groups_run="X25519"
else
	keys="p256 p384 rsa2048 rsa4096"
	suites13_run=$suites13
	groups_run="X25519 P-256"
fi

# 3a. Version x suite x group x key; the server prints its suite names the IANA way for both versions.
# TLS 1.2 with an ECDSA P-384 leaf is a known limit for now: RFC 8422 5.1 lets a TLS 1.2 server use only an ECDSA
# certificate on a curve the client lists in supported_groups, and the client lists x25519 and secp256r1 only; the
# server must refuse it for no shared cipher (the client sees handshake_failure, reason 1040).
for key in $keys; do
	for group in $groups_run; do
		if [ "$group" = P-256 ]; then
			retry=hrr
			code=23
		else
			retry=nohrr
			code=29
		fi
		for suite in $suites13_run; do
			interop "1.3 $suite $group $key" $key "-tls1_3 -ciphersuites $suite -groups $group" "-s 200" \
			    v13 "suite=$suite" "group=$code" $retry "scheme=$(scheme_of $key)"
		done
		set -- $(suites12_of $key)
		[ "$system" = win32 ] && set -- "$2"
		for suite in "$@"; do
			if [ "$key" = p384 ]; then
				case_limit=yes
				interop "1.2 $suite $group $key" $key "-tls1_2 -cipher $suite -groups $group" "-x reason=1040" \
				    fail no-shared-cipher
				case_limit=
				continue
			fi
			interop "1.2 $suite $group $key" $key "-tls1_2 -cipher $suite -groups $group" "-s 200" \
			    v12 "suite=$suite" "group=$code"
		done
	done
done

# 3b. Every signature scheme the client offers, with the server held to it (RSA: PSS in both versions, PKCS #1 v1.5
# in TLS 1.2; ECDSA in TLS 1.2 takes either hash with either curve).
if [ "$system" != win32 ]; then
	for key in rsa2048 rsa4096; do
		for scheme in rsa_pss_rsae_sha256 rsa_pss_rsae_sha384 rsa_pss_rsae_sha512; do
			interop "1.3 $scheme $key" $key "-tls1_3 -sigalgs $scheme" "" v13 "scheme=$scheme"
		done
		for scheme in rsa_pss_rsae_sha256 rsa_pss_rsae_sha384 rsa_pss_rsae_sha512 \
		    rsa_pkcs1_sha256 rsa_pkcs1_sha384 rsa_pkcs1_sha512; do
			interop "1.2 $scheme $key" $key "-tls1_2 -sigalgs $scheme" "" v12 "scheme=$scheme"
		done
	done
	for scheme in ecdsa_secp256r1_sha256 ecdsa_secp384r1_sha384; do
		interop "1.2 $scheme p256" p256 "-tls1_2 -sigalgs $scheme" "" v12 "scheme=$scheme"
		case_limit=yes
		interop "1.2 $scheme p384" p384 "-tls1_2 -sigalgs $scheme" "-x reason=1040" fail no-shared-cipher
		case_limit=
	done
fi

# 3c. Records: the server's records of at most 512 bytes (its certificates and its answer in many records), a long
# line read through a small buffer (SSL_pending), a non-blocking socket, the client's version pins.
for key in $keys; do
	interop "1.3 fragments of 512 $key" $key "-tls1_3 -max_send_frag 512" "-s 12000 -r 300 -P" v13
	interop "1.3 small reads $key" $key "-tls1_3" "-s 12000 -r 999 -P" v13
	interop "1.3 non-blocking $key" $key "-tls1_3 -max_send_frag 512" "-n -s 12000 -r 999" v13
	interop "1.3 non-blocking hrr $key" $key "-tls1_3 -groups P-256" "-n" v13 hrr
	[ "$key" = p384 ] && continue
	interop "1.2 fragments of 512 $key" $key "-tls1_2 -max_send_frag 512" "-s 12000 -r 300 -P" v12
	interop "1.2 non-blocking $key" $key "-tls1_2 -groups P-256" "-n -s 5000 -r 777 -P" v12 group=23
done
interop "client pinned to 1.2" p256 "" "-v 1.2" v12
interop "client pinned to 1.3" p256 "" "-v 1.3" v13
interop "1.3 by X509_VERIFY_PARAM" rsa2048 "-tls1_3" "-p" v13

# 3d. The end of a connection: the server's close_notify (the line CLOSE), the server gone without one (an error,
# or a clean end with SSL_OP_IGNORE_UNEXPECTED_EOF), the client's own close_notify.
for version in 1.3 1.2; do
	flag=-tls1_3
	[ $version = 1.2 ] && flag=-tls1_2
	interop "$version close_notify from the server" p256 "$flag" "-c notify"
	interop "$version eof without close_notify" p256 "$flag" "-c eof"
	interop "$version eof without close_notify, ignored" p256 "$flag" "-c eof-ok"
	interop "$version close_notify from the client" rsa2048 "$flag" "-c client"
done

# 3e. Handshakes that must fail: a root the client does not trust (20, unable to get local issuer), another name
# (62, hostname mismatch), an RSA leaf of 1024 bits (66, key too small; and without verification the signature
# check refuses the key: the client's alert ends the handshake).
for version in 1.3 1.2; do
	flag=-tls1_3
	[ $version = 1.2 ] && flag=-tls1_2
	interop "$version untrusted root" other "$flag" "-x verify=20" fail
	interop "$version RSA 1024 leaf" rsa1024 "$flag -cipher DEFAULT@SECLEVEL=0" "-x verify=66" fail
	interop "$version RSA 1024 leaf without verification" rsa1024 "$flag -cipher DEFAULT@SECLEVEL=0" "-k -x reason=134" fail alert=43
done
case_host=other.test
interop "1.3 another host name" p256 "-tls1_3" "-x verify=62" fail
case_host=

# 4. Windows: the trust sources of SSL_CTX_set_default_verify_paths without a CA file.  cert.pem beside the .exe
# (the test roots) is read, and none beside it leaves the store without them; the test intermediate (P-256) in the
# current user's Disallowed store refuses every chain through it (28, certificate rejected) and only that one, and
# only once the default paths are read.  The Disallowed entry is removed first (a run stopped before its end
# leaves it in the prefix) and at the end.
if [ "$system" = win32 ]; then
	der="Z:$top/$pki/int-p256.der"
	store_tool() {
		(cd "$out/x86_64" && LC_ALL=C.UTF-8 WINEPREFIX=$wine_prefix WINEDEBUG=-all WINEDLLOVERRIDES=winedbg.exe=d \
		    timeout 120 wine "$top/$out/x86_64/win-store.exe" "$@") >> "$out/win-store.txt" 2>&1
	}
	store_step() {
		cases=$((cases + 1))
		if store_tool "$1" "$der"; then
			echo "interop: win-store $1 the intermediate in Disallowed: PASS"
		else
			failed=$((failed + 1))
			echo "interop: win-store $1 the intermediate in Disallowed: FAIL ($out/win-store.txt)"
		fi
	}
	trap 'store_tool remove "$der"' EXIT
	store_step remove
	case_ca=none
	case_cert_pem=yes
	interop "default paths: cert.pem beside the .exe" p256 "-tls1_3" "-d" v13
	case_cert_pem=
	interop "default paths without cert.pem" p256 "-tls1_3" "-d -x verify=20" fail
	store_step add
	case_cert_pem=yes
	interop "1.3 Disallowed intermediate refused" p256 "-tls1_3" "-d -x verify=28" fail
	interop "1.2 Disallowed intermediate refused" p256 "-tls1_2" "-d -x verify=28" fail
	interop "Disallowed: a chain not through it" rsa2048 "-tls1_3" "-d" v13
	case_cert_pem=
	case_ca=
	interop "Disallowed: not read without the default paths" p256 "-tls1_3" "" v13
	store_step remove
	case_ca=none
	case_cert_pem=yes
	interop "default paths after the removal" p256 "-tls1_3" "-d" v13
	case_ca=
	case_cert_pem=
fi

echo "run-interop: summary"
if [ "$failed" -eq 0 ]; then
	echo "run-interop: PASS ($cases cases, $limits of them known limits)"
	exit 0
fi
echo "run-interop: FAIL ($failed of $cases cases)"
exit 1
