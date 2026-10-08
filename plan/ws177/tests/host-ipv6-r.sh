#!/bin/sh
# ws177-p044 to p046 (case R): builds host-ipv6-r.c with the parts of the libc resolver, networkd and dhcpc it checks
# (the host's headers, plain and ASan+UBSan) and runs it.
#   sh plan/ws177/tests/host-ipv6-r.sh [OUTPUT]   (default build/ws177-ipv6-r)
# Each run gets a new directory behind OUTPUT (plan/tools/fresh-out.sh); nothing is removed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws177-ipv6-r}
. plan/tools/fresh-out.sh
fresh_out "$out"
sources="plan/ws177/tests/host-ipv6-r.c userland/base/libc/resolver-dns.c userland/base/networkd/slaac.c userland/base/common/sha256.c
	userland/base/networkd/resolver6.c userland/base/net/netconf.c userland/base/net/reconcile.c"
status=0
for variant in plain asan; do
	flags="-std=c11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -I."
	[ "$variant" = asan ] && flags="$flags -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
	# shellcheck disable=SC2086
	${CC:-cc} $flags -o "$out/host-ipv6-r-$variant" $sources
	if "$out/host-ipv6-r-$variant" > "$out/ipv6-r-$variant.log" 2>&1; then
		echo "host-ipv6-r $variant: $(tail -1 "$out/ipv6-r-$variant.log")"
	else
		grep -E "^(FAILED|host-ipv6-r)" "$out/ipv6-r-$variant.log" || tail -5 "$out/ipv6-r-$variant.log"
		status=1
	fi
done
[ $status -eq 0 ] && echo "host-ipv6-r: PASS" || echo "host-ipv6-r: FAIL"
exit $status
