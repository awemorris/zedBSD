#!/bin/sh
# The host test of fetch's https (userland/base/fetch/main.c): fetch built for the host (ASan and UBSan) with
# libssl-compat and libcrypto-compat as static archives (plan/tools/tls/host-compat-libs.sh), against the loopback
# server https-server.py (the host's Python ssl).  The cases: bodies with a length, in chunks and to the end with and
# without close_notify; redirections absolute, relative and from http to https; a loop of redirections; a chain that
# does not verify (no CA file, a self-signed certificate, an expired one, another name); TLS 1.2; an address literal;
# plain http; a 404; a CA file that cannot be read.  A body is checked with SHA-256, and a failure leaves no file.
#   sh plan/ws343/tests/run-fetch-https.sh
# Output under build/tmp/ws343-fetch-https (a fresh run).  The last line is run-fetch-https: PASS (<N> cases) or
# run-fetch-https: FAIL.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
PATH=/usr/bin:/bin:$PATH
export PATH
python=/usr/bin/python3
. plan/tools/fresh-out.sh
out=build/tmp/ws343-fetch-https
fresh_out "$out"
status=0
server=

# 1. The compat libraries for the host (kept between runs under build/tmp).
compat=build/tmp/ws343-compat
if ! timeout 600 sh plan/tools/tls/host-compat-libs.sh "$compat" > "$out/compat-libs.log" 2>&1; then
	tail -20 "$out/compat-libs.log"
	echo "run-fetch-https: FAIL (the compat libraries; $out/compat-libs.log)"
	exit 1
fi

# 2. fetch for the host.
if ! timeout 300 cc -std=gnu17 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I. -I"$compat/include" \
    userland/base/fetch/main.c "$compat/libssl-compat.a" "$compat/libcrypto-compat.a" -lpthread \
    -o "$out/fetch" > "$out/build.log" 2>&1 || [ -s "$out/build.log" ]; then
	head -20 "$out/build.log"
	echo "run-fetch-https: FAIL (the build; $out/build.log)"
	exit 1
fi

# 3. The certificates: a CA with leaves for localhost and 127.0.0.1, for another name and an expired one; a self-signed one.
pki=$out/pki
mkdir -p "$pki"
leaf() {
	openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj "/CN=$1" \
	    -keyout "$pki/$2.key" -out "$pki/$2.csr" &&
	printf 'subjectAltName=%s\nbasicConstraints=CA:FALSE\n' "$3" > "$pki/$2.ext" &&
	openssl x509 -req -in "$pki/$2.csr" -CA "$pki/ca.pem" -CAkey "$pki/ca.key" -CAcreateserial \
	    -extfile "$pki/$2.ext" $4 -out "$pki/$2.pem"
}
{
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj /CN=fetch-test-ca \
	    -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign \
	    -keyout "$pki/ca.key" -out "$pki/ca.pem" &&
	leaf localhost good "DNS:localhost,IP:127.0.0.1" "-days 2" &&
	leaf other.example othername "DNS:other.example" "-days 2" &&
	leaf localhost expired "DNS:localhost" "-not_before 20200101000000Z -not_after 20200102000000Z" &&
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj /CN=localhost \
	    -addext subjectAltName=DNS:localhost -keyout "$pki/self.key" -out "$pki/self.pem"
} > "$pki/openssl.log" 2>&1 || { tail -10 "$pki/openssl.log"; echo "run-fetch-https: FAIL (certificates)"; exit 1; }

# 4. The body and the server.
head -c 300000 /dev/urandom > "$out/body"
want=$(sha256sum "$out/body" | cut -d' ' -f1)
"$python" -I plan/ws343/tests/https-server.py "$out/ports" "$out/body" "$pki/good.pem" "$pki/good.key" \
    "$pki/othername.pem" "$pki/othername.key" othername "$pki/expired.pem" "$pki/expired.key" expired \
    "$pki/self.pem" "$pki/self.key" self > "$out/server.log" 2>&1 &
server=$!
trap 'kill $server 2> /dev/null' EXIT
tries=0
while [ ! -f "$out/ports" ] && [ $tries -lt 100 ]; do
	tries=$((tries + 1))
	"$python" -I -c 'import time; time.sleep(0.1)'
done
[ -f "$out/ports" ] || { cat "$out/server.log"; echo "run-fetch-https: FAIL (the server did not start)"; exit 1; }
port() {
	awk -v n="$1" '$1 == n { print $2 }' "$out/ports"
}
tls=$(port tls)
tls12=$(port tls12)
plain=$(port plain)

# 5. The cases.  ok <label> <url> [options...]: the body arrives whole.  fail <label> <text> <url> [options...]: fetch
# fails, says <text> and leaves no file.
count=0
ok() {
	label=$1
	url=$2
	shift 2
	count=$((count + 1))
	file=$out/case-$count.out
	timeout 60 "$out/fetch" "$@" -o "$file" "$url" > "$out/case-$count.log" 2>&1
	code=$?
	got=
	[ -f "$file" ] && got=$(sha256sum "$file" | cut -d' ' -f1)
	if [ "$code" = 0 ] && [ "$got" = "$want" ]; then
		echo "fetch-https: $label: PASS"
	else
		echo "fetch-https: $label: FAIL (exit $code)"
		tail -5 "$out/case-$count.log"
		status=1
	fi
}
fail() {
	label=$1
	text=$2
	url=$3
	shift 3
	count=$((count + 1))
	file=$out/case-$count.out
	timeout 60 "$out/fetch" "$@" -o "$file" "$url" > "$out/case-$count.log" 2>&1
	code=$?
	if [ "$code" = 1 ] && [ ! -e "$file" ] && grep -q -- "$text" "$out/case-$count.log"; then
		echo "fetch-https: $label: PASS ($(grep -- "$text" "$out/case-$count.log" | head -1))"
	else
		echo "fetch-https: $label: FAIL (exit $code)"
		tail -5 "$out/case-$count.log"
		status=1
	fi
}
ca="-C $pki/ca.pem"
ok "length" "https://localhost:$tls/file" $ca
ok "chunked" "https://localhost:$tls/chunked" $ca
ok "to the end with close_notify" "https://localhost:$tls/eof-notify" $ca
fail "to the end without close_notify" "TLS read failed" "https://localhost:$tls/eof-cut" $ca
ok "absolute-path redirect" "https://localhost:$tls/abs" $ca
ok "relative-path redirect" "https://localhost:$tls/dir/rel" $ca
ok "http to https redirect" "http://localhost:$plain/to-https" $ca
fail "redirect loop" "too many redirects" "https://localhost:$tls/loop" $ca
fail "no CA file" "certificate verify failed" "https://localhost:$tls/file"
fail "self-signed" "certificate verify failed" "https://localhost:$(port self)/file" $ca
fail "expired" "certificate verify failed" "https://localhost:$(port expired)/file" $ca
fail "another name" "certificate verify failed" "https://localhost:$(port othername)/file" $ca
ok "TLS 1.2" "https://localhost:$tls12/file" $ca
ok "address literal" "https://127.0.0.1:$tls/file" $ca
ok "plain http" "http://localhost:$plain/file"
fail "404" "HTTP status 404" "https://localhost:$tls/missing" $ca
fail "unreadable CA file" "cannot read the CA file" "https://localhost:$tls/file" -C "$pki/none.pem"

# The standard output (-o -).
count=$((count + 1))
timeout 60 "$out/fetch" -q $ca -o - "https://localhost:$tls/file" > "$out/case-$count.out" 2> "$out/case-$count.log"
got=$(sha256sum "$out/case-$count.out" | cut -d' ' -f1)
if [ "$got" = "$want" ]; then
	echo "fetch-https: standard output: PASS"
else
	echo "fetch-https: standard output: FAIL"
	status=1
fi

if [ $status -eq 0 ]; then
	echo "run-fetch-https: PASS ($count cases)"
else
	echo "run-fetch-https: FAIL"
fi
exit $status
