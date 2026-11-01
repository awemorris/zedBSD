#!/bin/sh
# https of libbrowser on Windows under Wine (x86_64), with the OpenSSL linked into the program: the
# test program plan/ws334/tests/tls-main.c (built by the Windows build's test-program, which links the
# static OpenSSL) fetches a page from the host's `openssl s_server` on 127.0.0.1 (a test CA and a
# server certificate made here, the network outside is not used), with the blocking fetch and with the
# loader, and must:
#   a. connect when the test CA is added with --ca-file;
#   b. fail the verification when nothing trusts the CA;
#   c. connect when the CA is in Wine's current-user ROOT store (the program puts it there for the run);
#   d. refuse when the same CA is also in the Disallowed store;
#   e. connect when the CA is the bundle of roots shipped with the program (share/certs/ca-certificates.pem
#      beside the .exe);
#   f. still connect with OPENSSL_CONF naming a configuration that would break TLS if it were read (it
#      activates only the base provider, which has no algorithms); the program reads no configuration.
# It also checks that the program holds the --openssldir path no user can create.
#   sh plan/ws334/tests/tls-wine.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64);
# WINEPREFIX=<dir> the Wine prefix (default build/tmp/ws334-tls-wine); PORT=<n> the server's port (18443).
# Wine runs under flock /tmp/zedbsd-wine.lock.  Last line: tls-wine: PASS or tls-wine: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ws334-tls-wine
fresh_out "$out"
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
WINEPREFIX=${WINEPREFIX:-$(pwd)/build/tmp/ws334-tls-wine-prefix}
export WINEPREFIX
port=${PORT:-18443}
build=${BROWSER_WIN32_BUILD:-build/tmp/ws334-tls-build/browser-win64}
sources=build/tmp/ws334-win32-sources
mkdir -p "$sources"
status=0

# The program, with the OpenSSL it links.
if ! timeout 3300 make --no-print-directory -f apps/browser-win32/Makefile WIN32_ARCH=x86_64 \
    LLVM_MINGW="$LLVM_MINGW" BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" -j8 \
    test-program TEST_PROGRAM=tls-tool TEST_PROGRAM_SOURCES="plan/ws334/tests/tls-main.c" \
    > "$out/make.log" 2>&1 || grep -q 'warning:' "$out/make.log"; then
	echo "tls-wine: the program did not build cleanly (log $out/make.log)"
	tail -10 "$out/make.log"
	echo "tls-wine: FAIL"
	exit 1
fi
program=$build/tests/tls-tool.exe

# The path no user can create is in the program.
if strings -a "$program" | grep -q 'C:/Windows/System32/OmniGrammaton-no-config'; then
	echo "tls-wine: the program holds the configured openssldir"
else
	echo "tls-wine: the program does not hold the configured openssldir"
	status=1
fi

# A test CA, a server certificate for 127.0.0.1 signed by it, and the CA in DER for the store.
mkdir -p "$out/pki" "$out/site"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=tls-wine test CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -keyout "$out/pki/ca.key" -out "$out/pki/ca.pem" > "$out/pki.log" 2>&1
openssl req -newkey rsa:2048 -nodes -subj "/CN=127.0.0.1" -keyout "$out/pki/server.key" \
    -out "$out/pki/server.csr" >> "$out/pki.log" 2>&1
printf 'subjectAltName=IP:127.0.0.1\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n' > "$out/pki/server.ext"
openssl x509 -req -in "$out/pki/server.csr" -CA "$out/pki/ca.pem" -CAkey "$out/pki/ca.key" -CAcreateserial \
    -days 2 -extfile "$out/pki/server.ext" -out "$out/pki/server.pem" >> "$out/pki.log" 2>&1
openssl x509 -in "$out/pki/ca.pem" -outform DER -out "$out/pki/ca.der" >> "$out/pki.log" 2>&1
if [ ! -s "$out/pki/server.pem" ] || [ ! -s "$out/pki/ca.der" ]; then
	echo "tls-wine: cannot make the test certificates (log $out/pki.log)"
	echo "tls-wine: FAIL"
	exit 1
fi

# The page: 20000 bytes of text, so that it comes in several records.
/usr/bin/python3 -I -c 'import sys; sys.stdout.write("".join("line %05d of the page\n" % i for i in range(20000 // 22)))' \
    > "$out/site/page.txt"
size=$(wc -c < "$out/site/page.txt" | tr -d ' ')

# The server, for as long as the program runs (-WWW serves files below its directory).
(cd "$out/site" && exec openssl s_server -accept "127.0.0.1:$port" -cert ../pki/server.pem -key ../pki/server.key \
    -WWW -quiet > ../server.log 2>&1) &
server=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
	if /usr/bin/python3 -I -c "import socket,sys; s=socket.socket(); s.settimeout(1); sys.exit(s.connect_ex(('127.0.0.1', $port)))"; then
		break
	fi
	sleep 1
done
url="https://127.0.0.1:$port/page.txt"

# One run of the program under Wine: run <name> <program> <options...>; its output in $out/<name>.txt.
run() {
	name=$1
	shift
	WINEDEBUG=-all timeout 300 flock /tmp/zedbsd-wine.lock wine "$@" "$url" > "$out/$name.out" 2> "$out/$name.log"
	tr -d '\r' < "$out/$name.out" > "$out/$name.txt"
	echo "--- $name"
	cat "$out/$name.txt"
}

# Both fetches got the page.
connects() {
	for way in blocking loader; do
		if ! grep -qx "tls-tool: $way 200 $size" "$out/$1.txt"; then
			echo "tls-wine: $1: the $way fetch did not give 200 and $size bytes"
			status=1
		fi
	done
}

# Both fetches failed.
fails() {
	for way in blocking loader; do
		if ! grep -q "^tls-tool: $way error" "$out/$1.txt"; then
			echo "tls-wine: $1: the $way fetch did not fail"
			status=1
		fi
	done
}

# a. the CA added as a file.
run a-ca-file "$program" --ca-file "$out/pki/ca.pem"
connects a-ca-file

# b. nothing trusts the CA.
run b-untrusted "$program"
fails b-untrusted
if ! grep -q 'certificate verify failed' "$out/b-untrusted.txt"; then
	echo "tls-wine: b-untrusted: the failure is not a failed verification"
	status=1
fi

# c. the CA in the ROOT store.
run c-root "$program" --root "$out/pki/ca.der"
connects c-root

# d. the CA in ROOT and in Disallowed.
run d-disallowed "$program" --root "$out/pki/ca.der" --disallowed "$out/pki/ca.der"
fails d-disallowed
if ! grep -q 'certificate rejected' "$out/d-disallowed.txt"; then
	echo "tls-wine: d-disallowed: the failure is not the certificate being rejected"
	status=1
fi

# e. the CA as the bundle beside the program (a copy of the program in its own directory).
mkdir -p "$out/bundle/share/certs"
cp "$program" "$out/bundle/tls-tool.exe"
cp "$out/pki/ca.pem" "$out/bundle/share/certs/ca-certificates.pem"
run e-bundle "$out/bundle/tls-tool.exe"
connects e-bundle

# f. OPENSSL_CONF naming a configuration that would break TLS if it were read (with the CA added, as in
# a): only the base provider is activated, so the default provider's algorithms are never loaded.
printf 'openssl_conf = init\n[init]\nproviders = providers\n[providers]\nbase = base_section\n[base_section]\nactivate = 1\n' \
    > "$out/bad-openssl.cnf"
OPENSSL_CONF="$out/bad-openssl.cnf" run f-conf "$program" --ca-file "$out/pki/ca.pem"
connects f-conf

kill "$server" 2> /dev/null
wait "$server" 2> /dev/null
sync
if [ "$status" = 0 ]; then echo "tls-wine: PASS"; else echo "tls-wine: FAIL"; fi
exit $status
