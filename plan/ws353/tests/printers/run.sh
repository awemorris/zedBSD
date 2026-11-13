#!/bin/sh
# Builds and runs the host test of the printers' deadline (libkeiland-backend/print/print.c, kl_backend_print_due)
# under ASan+UBSan and TSan, and the settings file's host test it shares the backend with
# (plan/ws309/tests/print-file/run.sh).
#   sh plan/ws353/tests/printers/run.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../../.."
out=${1:-build/ws353/host-printers}
mkdir -p "$out"
for sanitizer in address,undefined thread; do
	${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -D_GNU_SOURCE -I. -fsanitize=$sanitizer -g -pthread \
	    userland/desktop/libkeiland-backend/print/print.c plan/ws353/tests/printers/host-print-due.c \
	    -o "$out/host-print-due-${sanitizer%%,*}"
	echo "== sanitizer $sanitizer"
	TEST_DIR="$(pwd)/$out" timeout 60 "$out/host-print-due-${sanitizer%%,*}"
done
sh plan/ws309/tests/print-file/run.sh "$out/print-file"
