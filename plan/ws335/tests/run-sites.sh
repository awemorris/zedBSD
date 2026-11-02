#!/bin/sh
# Real sites from the host through libssl-compat: tls-client.c (built as run-interop.sh builds it for the host)
# fetches "/" of each site below over HTTPS with the host's CA bundle and must see an HTTP answer; the badssl.com
# cases must fail with the given verify result.  The sites are outside this project, so the result is a record, not
# an acceptance (a site that changes its certificate or goes away changes the outcome).  Without a network (the
# first name does not resolve) every case is SKIP.
#   sh plan/ws335/tests/run-sites.sh [CA-FILE]     (default /etc/ssl/certs/ca-certificates.crt)
# Each case prints "sites: <name>: PASS", "FAIL (<why>; <log>)" or "SKIP"; the last line is
# "run-sites: <passed> PASS, <failed> FAIL, <skipped> SKIP".  Output under build/tmp/ws335-sites (a fresh run).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
PATH=/usr/bin:/bin:$PATH
export PATH
. plan/tools/fresh-out.sh
out=build/tmp/ws335-sites
fresh_out "$out"
ca=${1:-/etc/ssl/certs/ca-certificates.crt}
mkdir -p "$out/include"
ln -sfn "$(pwd)/include/libc/compat" "$out/include/compat"
modules=
for file in userland/base/libcrypto-compat/*.c userland/base/libssl-compat/*.c; do
	case "$file" in
	*-win32.c) ;;
	*) modules="$modules $file" ;;
	esac
done
if ! timeout 600 cc -std=c89 -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -fsanitize=address,undefined \
    -fno-sanitize-recover=all -fno-omit-frame-pointer -I"$out/include" -I. plan/ws335/tests/tls-client.c $modules \
    -lpthread -o "$out/tls-client" > "$out/build.log" 2>&1 || [ -s "$out/build.log" ]; then
	head -20 "$out/build.log"
	echo "run-sites: FAIL (client build; $out/build.log)"
	exit 1
fi

network=yes
timeout 10 getent hosts www.google.com > /dev/null 2>&1 || network=no
passed=0
failed=0
skipped=0
number=0

# One case: a name, the host, the port, then tls-client's options.
site() {
	name=$1
	host=$2
	port=$3
	shift 3
	number=$((number + 1))
	if [ "$network" = no ]; then
		skipped=$((skipped + 1))
		echo "sites: $name: SKIP"
		return
	fi
	log=$out/site-$number.txt
	timeout 60 "$out/tls-client" -t 20 "$@" "$host" "$port" "$ca" > "$log" 2>&1
	last=$(tail -1 "$log")
	if [ "$last" = "tls-client: PASS" ]; then
		passed=$((passed + 1))
		echo "sites: $name: PASS ($(grep -m1 '^tls-client: HTTP/' "$log" | sed 's/^tls-client: //'))"
	else
		failed=$((failed + 1))
		echo "sites: $name: FAIL ($last; $log)"
	fi
}

# Web PKI: RSA and ECDSA leaves, P-384 intermediates (Let's Encrypt E-series), several CAs and CDNs.
site "Google" www.google.com 443 -g
site "Google, TLS 1.2" www.google.com 443 -g -v 1.2
site "Cloudflare" www.cloudflare.com 443 -g
site "Cloudflare, TLS 1.2" www.cloudflare.com 443 -g -v 1.2
site "GitHub" github.com 443 -g
site "Wikipedia" www.wikipedia.org 443 -g
site "Let's Encrypt" letsencrypt.org 443 -g
site "ISRG Root X1 (RSA)" valid-isrgrootx1.letsencrypt.org 443 -g
site "ISRG Root X2 (ECDSA P-384)" valid-isrgrootx2.letsencrypt.org 443 -g
site "Amazon" www.amazon.com 443 -g
site "Microsoft" www.microsoft.com 443 -g
site "Apple" www.apple.com 443 -g
site "Mozilla" www.mozilla.org 443 -g
site "Debian" www.debian.org 443 -g
site "kernel.org" www.kernel.org 443 -g
site "Fastly" www.fastly.com 443 -g
site "Akamai" www.akamai.com 443 -g
site "DigiCert" www.digicert.com 443 -g
site "Sectigo" www.sectigo.com 443 -g
site "badssl ECDSA P-256" ecc256.badssl.com 443 -g
site "badssl ECDSA P-384" ecc384.badssl.com 443 -g
site "badssl RSA 2048" rsa2048.badssl.com 443 -g
site "badssl RSA 4096" rsa4096.badssl.com 443 -g
site "badssl SHA-384" sha384.badssl.com 443 -g
site "badssl TLS 1.2 only" tls-v1-2.badssl.com 1012 -g

# Failures the client must refuse: expired (10), another name (62), self-signed (18), an unknown root (19 or 20).
site "badssl expired" expired.badssl.com 443 -x verify=10
site "badssl wrong host" wrong.host.badssl.com 443 -x verify=62
site "badssl self-signed" self-signed.badssl.com 443 -x verify=18
site "badssl untrusted root" untrusted-root.badssl.com 443 -x verify=19

echo "run-sites: $passed PASS, $failed FAIL, $skipped SKIP"
