#!/bin/sh
# https of libbrowser on Windows under Wine (x86_64) with libssl-compat and libcrypto-compat linked into the
# program in place of OpenSSL (net/tls-static-compat.c fills tls.c's table).  The same cases as
# plan/ws334/tests/tls-wine.sh, with its test program plan/ws334/tests/tls-main.c, against the host's
# `openssl s_server` on 127.0.0.1 (a test CA and a server certificate made here):
#   a. connect when the test CA is added with --ca-file;
#   b. fail the verification when nothing trusts the CA;
#   c. connect when the CA is in Wine's current-user ROOT store;
#   d. refuse ("certificate rejected") when the same CA is also in the Disallowed store;
#   e. connect when the CA is the bundle beside the program (share/certs/ca-certificates.pem);
#   f. still connect with OPENSSL_CONF naming a configuration that would break OpenSSL (nothing reads it);
#   g. connect when the CA is cert.pem beside the .exe (libssl-compat's default paths on Windows);
# and the program must import no DLL of OpenSSL and hold none of OpenSSL's names, and hold libssl-compat's.
#
# apps/browser-win32/Makefile links OpenSSL today.  Until it links the compat libraries itself, the program is built
# with the proposed change (plan/ws335/phase036/browser-win32-Makefile.diff) applied to a copy of the Makefile in the
# run directory (make -f that copy, from the top of the tree); the tree's Makefile is not changed.  With
# COMPAT_BY_MAKEFILE=1 (once the Makefile has the change) the tree's Makefile is used.
#   sh plan/ws335/tests/tls-wine-compat.sh
# LLVM_MINGW=<root> chooses llvm-mingw (default $HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64);
# WINEPREFIX=<dir> the Wine prefix (default build/tmp/ws335-tls-wine-prefix); PORT=<n> the server's port (18453).
# Wine runs under flock /tmp/zedbsd-wine.lock; make runs with -j4.  Last line: tls-wine-compat: PASS or FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
top=$(pwd)
. plan/tools/fresh-out.sh
out=build/tmp/ws335-tls-wine-compat
fresh_out "$out"
LLVM_MINGW=${LLVM_MINGW:-$HOME/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64}
WINEPREFIX=${WINEPREFIX:-$top/build/tmp/ws335-tls-wine-prefix}
export WINEPREFIX
port=${PORT:-18453}
build=${BROWSER_WIN32_BUILD:-build/tmp/ws335-tls-build/browser-win64}
sources=build/tmp/ws334-win32-sources
wcc=$LLVM_MINGW/bin/x86_64-w64-mingw32-clang
mkdir -p "$sources"
status=0

# 1. The Makefile: the tree's, or a copy with the proposed change.
makefile=apps/browser-win32/Makefile
if [ "${COMPAT_BY_MAKEFILE:-0}" != 1 ]; then
	makefile=$out/Makefile
	cp apps/browser-win32/Makefile "$makefile"
	if ! patch -s "$makefile" < plan/ws335/phase036/browser-win32-Makefile.diff > "$out/patch.log" 2>&1; then
		cat "$out/patch.log"
		echo "tls-wine-compat: the proposed change does not apply to apps/browser-win32/Makefile"
		echo "tls-wine-compat: FAIL"
		exit 1
	fi
fi
if grep -qi 'openssl' "$makefile"; then
	echo "tls-wine-compat: $makefile still names OpenSSL"
	status=1
fi

# 2. The program, and the two compat archives the Makefile makes for it.
if ! timeout 3300 make --no-print-directory -f "$makefile" WIN32_ARCH=x86_64 LLVM_MINGW="$LLVM_MINGW" \
    BROWSER_WIN32_BUILD="$build" BROWSER_WIN32_SOURCES="$sources" -j4 \
    test-program TEST_PROGRAM=tls-tool TEST_PROGRAM_SOURCES="plan/ws334/tests/tls-main.c" > "$out/make.log" 2>&1 ||
    grep -q 'warning:' "$out/make.log"; then
	echo "tls-wine-compat: the program did not build cleanly (log $out/make.log)"
	tail -10 "$out/make.log"
	echo "tls-wine-compat: FAIL"
	exit 1
fi
for archive in libssl-compat.a libcrypto-compat.a; do
	if [ ! -f "$build/obj/$archive" ]; then
		echo "tls-wine-compat: the build made no $archive"
		status=1
	fi
done
program=$build/tests/tls-tool.exe
if [ ! -f "$build/obj/userland/desktop/libbrowser/net/tls-static-compat.o" ]; then
	echo "tls-wine-compat: the program was not built with net/tls-static-compat.c"
	status=1
fi

# 3. What the program is made of: no OpenSSL DLL among its imports, none of OpenSSL's names, libssl-compat's.
"$LLVM_MINGW/bin/llvm-objdump" -p "$program" | sed -n 's/^ *DLL Name: //p' | tr 'A-Z' 'a-z' > "$out/imports.txt"
echo "tls-wine-compat: imports: $(tr '\n' ' ' < "$out/imports.txt")"
if grep -q 'ssl\|crypto-' "$out/imports.txt"; then
	echo "tls-wine-compat: the program imports a DLL of OpenSSL"
	status=1
fi
"$LLVM_MINGW/bin/llvm-nm" "$program" > "$out/names.txt" 2>&1
if grep -q ' OPENSSL_init_ssl$\| X509_digest$\| X509_STORE_CTX_set_error$' "$out/names.txt"; then
	echo "tls-wine-compat: the program holds OpenSSL's own functions"
	status=1
fi
if ! grep -q ' SSL_connect$' "$out/names.txt" || ! grep -q ' sslc_' "$out/names.txt"; then
	echo "tls-wine-compat: the program does not hold libssl-compat"
	status=1
fi

# 4. A test CA, a server certificate for 127.0.0.1 signed by it, and the CA in DER for the stores.
mkdir -p "$out/pki" "$out/site"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=tls-wine-compat test CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -keyout "$out/pki/ca.key" -out "$out/pki/ca.pem" > "$out/pki.log" 2>&1
openssl req -newkey rsa:2048 -nodes -subj "/CN=127.0.0.1" -keyout "$out/pki/server.key" \
    -out "$out/pki/server.csr" >> "$out/pki.log" 2>&1
printf 'subjectAltName=IP:127.0.0.1\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n' > "$out/pki/server.ext"
openssl x509 -req -in "$out/pki/server.csr" -CA "$out/pki/ca.pem" -CAkey "$out/pki/ca.key" -CAcreateserial \
    -days 2 -extfile "$out/pki/server.ext" -out "$out/pki/server.pem" >> "$out/pki.log" 2>&1
openssl x509 -in "$out/pki/ca.pem" -outform DER -out "$out/pki/ca.der" >> "$out/pki.log" 2>&1
if [ ! -s "$out/pki/server.pem" ] || [ ! -s "$out/pki/ca.der" ]; then
	echo "tls-wine-compat: cannot make the test certificates (log $out/pki.log)"
	echo "tls-wine-compat: FAIL"
	exit 1
fi

# The page: 20000 bytes of text, so that it comes in several records.
/usr/bin/python3 -I -c 'import sys; sys.stdout.write("".join("line %05d of the page\n" % i for i in range(20000 // 22)))' \
    > "$out/site/page.txt"
size=$(wc -c < "$out/site/page.txt" | tr -d ' ')

# The server, for as long as the program runs (-WWW serves files below its directory).
(cd "$out/site" && exec timeout 1800 openssl s_server -accept "127.0.0.1:$port" -cert ../pki/server.pem \
    -key ../pki/server.key -WWW -quiet > ../server.log 2>&1) &
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
			echo "tls-wine-compat: $1: the $way fetch did not give 200 and $size bytes"
			status=1
		fi
	done
}

# Both fetches failed.
fails() {
	for way in blocking loader; do
		if ! grep -q "^tls-tool: $way error" "$out/$1.txt"; then
			echo "tls-wine-compat: $1: the $way fetch did not fail"
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
	echo "tls-wine-compat: b-untrusted: the failure is not a failed verification"
	status=1
fi

# c. the CA in the ROOT store.
run c-root "$program" --root "$out/pki/ca.der"
connects c-root

# d. the CA in ROOT and in Disallowed.
run d-disallowed "$program" --root "$out/pki/ca.der" --disallowed "$out/pki/ca.der"
fails d-disallowed
if ! grep -q 'certificate rejected' "$out/d-disallowed.txt"; then
	echo "tls-wine-compat: d-disallowed: the failure is not the certificate being rejected"
	status=1
fi

# e. the CA as the bundle beside the program (a copy of the program in its own directory).
mkdir -p "$out/bundle/share/certs"
cp "$program" "$out/bundle/tls-tool.exe"
cp "$out/pki/ca.pem" "$out/bundle/share/certs/ca-certificates.pem"
run e-bundle "$out/bundle/tls-tool.exe"
connects e-bundle

# f. OPENSSL_CONF naming a configuration that would break OpenSSL (with the CA added, as in a).
printf 'openssl_conf = init\n[init]\nproviders = providers\n[providers]\nbase = base_section\n[base_section]\nactivate = 1\n' \
    > "$out/bad-openssl.cnf"
OPENSSL_CONF="$out/bad-openssl.cnf" run f-conf "$program" --ca-file "$out/pki/ca.pem"
connects f-conf

# g. the CA as cert.pem beside the .exe (another copy in its own directory).
mkdir -p "$out/beside"
cp "$program" "$out/beside/tls-tool.exe"
cp "$out/pki/ca.pem" "$out/beside/cert.pem"
run g-cert-pem "$out/beside/tls-tool.exe"
connects g-cert-pem

kill "$server" 2> /dev/null
wait "$server" 2> /dev/null
sync
if [ "$status" = 0 ]; then echo "tls-wine-compat: PASS"; else echo "tls-wine-compat: FAIL"; fi
exit $status
