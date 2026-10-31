#!/bin/sh
# The shape checks of zedBSD's ssh, sshd and ssh-keygen sources (userland/base/ssh):
#  1. every header compiles alone (included twice, so its guard works), with cc and clang,
#     -std=c89 -Wall -Wextra -Werror -Wpedantic;
#  2. every .c compiles with both (the crypto headers from the host-OpenSSL shims);
#  3. only ssh-crypto.c names the crypto library (<compat/crypto/...> or <openssl/...>);
#  4. when libcrypto-compat's headers are present (include/libc/compat/crypto/evp.h), ssh-crypto.c also compiles
#     against them alone, so it uses nothing libcrypto-compat does not declare;
#  5. no source names development records (a WS, Phase, Queue or Bug number, or plan/);
#  6. with BASE=<commit>, the headers are unchanged since that commit (an implementation Phase must not change
#     them; a header fault goes back to Q1).
#   sh plan/ws336/tests/check-skeleton.sh
#   BASE=<sha> sh plan/ws336/tests/check-skeleton.sh
# Last line: check-skeleton: PASS or check-skeleton: FAIL.  Output under build/tmp/ssh-skeleton.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=build/tmp/ssh-skeleton
fresh_out "$out"
flags="-std=c89 -Wall -Wextra -Werror -Wpedantic -D_DEFAULT_SOURCE -I. -Iuserland/base/ssh"
status=0

for compiler in cc clang; do
	if ! command -v "$compiler" > /dev/null 2>&1; then
		echo "$compiler: skipped (not installed)"
		continue
	fi
	for header in userland/base/ssh/*.h; do
		printf '#include "%s"\n#include "%s"\n' "$header" "$header" > "$out/one.c"
		if ! timeout 120 $compiler $flags -fsyntax-only "$out/one.c" > "$out/header.log" 2>&1; then
			echo "FAILED $compiler $header"
			head -10 "$out/header.log"
			status=1
		fi
	done
	for source in userland/base/ssh/*.c; do
		if ! timeout 120 $compiler $flags -Iplan/ws336/tests/host-crypto -c "$source" \
		    -o "$out/$compiler-$(basename "$source" .c).o" > "$out/source.log" 2>&1; then
			echo "FAILED $compiler $source"
			head -10 "$out/source.log"
			status=1
		fi
	done
done

if grep -n -E '#[[:space:]]*include[[:space:]]*[<"](compat/crypto|openssl)/' userland/base/ssh/*.c userland/base/ssh/*.h \
    | grep -v '^userland/base/ssh/ssh-crypto\.c:'; then
	echo "FAILED a file other than ssh-crypto.c names the crypto library (above)"
	status=1
fi

if [ -f include/libc/compat/crypto/evp.h ]; then
	# Only compat/ is put on the include path, not the rest of zedBSD's C library headers, so the host's own
	# headers stay in use.
	mkdir -p "$out/compat-root"
	ln -sfn "$(pwd)/include/libc/compat" "$out/compat-root/compat"
	if ! timeout 120 cc $flags -I"$out/compat-root" -fsyntax-only userland/base/ssh/ssh-crypto.c > "$out/compat.log" 2>&1; then
		echo "FAILED ssh-crypto.c against libcrypto-compat's headers"
		head -20 "$out/compat.log"
		status=1
	fi
else
	echo "libcrypto-compat headers: not present (check 4 skipped)"
fi

if grep -n -E '\b(WS|ws)[0-9]{3}\b|\bq[0-9]{3,4}\b|BUG-[0-9]+|plan/' userland/base/ssh/*.c userland/base/ssh/*.h; then
	echo "FAILED development records named in the sources (above)"
	status=1
fi

if [ -n "${BASE:-}" ]; then
	if ! git diff --quiet "$BASE" -- 'userland/base/ssh/*.h'; then
		echo "FAILED headers changed since $BASE:"
		git diff --stat "$BASE" -- 'userland/base/ssh/*.h'
		status=1
	fi
fi

if [ "$status" = 0 ]; then echo "check-skeleton: PASS"; else echo "check-skeleton: FAIL"; fi
exit "$status"
