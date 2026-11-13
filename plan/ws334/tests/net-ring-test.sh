#!/bin/sh
# Builds and runs plan/ws334/tests/net-ring-test.c against libbrowser's network sources (with the storage
# queue and records the cookies use) and their POSIX operating-system layer (the host's C compiler): the
# loader's threads, net_loader_ring and the timed wait.
#   sh plan/ws334/tests/net-ring-test.sh [plain|tsan]
# The output goes to build/tmp/ws334-net-ring/<variant>/; the zlib headers come from the WS074 host build
# (sh plan/ws074/tests/host-build.sh plain makes build/ws074-host/include).
# Last line: net-ring-test: PASS, or the failures.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
cd "$(dirname -- "$0")/../../.."
variant=${1:-plain}
case $variant in
plain) sanitize= ;;
tsan) sanitize=-fsanitize=thread ;;
*) echo "net-ring-test: unknown variant $variant" >&2; exit 2 ;;
esac
out=build/tmp/ws334-net-ring/$variant
mkdir -p "$out"
src=userland/desktop/libbrowser
base=$(ls $src/base/*.c | grep -v -e '-win32\.c$')
# The TLS library (libcrypto-compat, libssl-compat), which net/tls.c calls directly, for the host.
HOST_COMPAT_CFLAGS=$sanitize sh plan/tools/tls/host-compat-libs.sh "$out/compat" || { echo "net-ring-test: FAIL (host-compat-libs.sh)"; exit 1; }
# The network's sources but the Windows build's old bindings of the TLS libraries, until they are removed.
net=$(ls $src/net/*.c | grep -v -e '/tls-static\.c$' -e '/tls-static-compat\.c$')
cc $sanitize -std=gnu11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I$src -I. -Iuserland/desktop/include -Ibuild/ws074-host/include -I$out/compat/include \
	$net $src/storage/queue.c $src/storage/records.c $base userland/base/libz-compat/*.c \
	plan/ws334/tests/net-ring-test.c userland/tests/browser-threads/threads-posix.c \
	"$out/compat/libssl-compat.a" "$out/compat/libcrypto-compat.a" -o "$out/net-ring-test" -ldl -lpthread -lm || exit 1
TSAN_OPTIONS="halt_on_error=1 exitcode=66" timeout 120 "$out/net-ring-test"
