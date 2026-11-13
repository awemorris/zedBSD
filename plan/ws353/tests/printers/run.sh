#!/bin/sh
# Builds and runs the host tests of the printers' deadline (libkeiland-backend/print/print.c, kl_backend_print_due) and
# of the settings file in a new home (its folders made) under ASan+UBSan and TSan, then the settings file's host test
# on the same backend (plan/ws309/tests/print-file/run.sh).
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
	${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -D_GNU_SOURCE -I. -fsanitize=$sanitizer -g -pthread \
	    userland/desktop/libkeiland-backend/print/print.c plan/ws353/tests/printers/host-print-folders.c \
	    -o "$out/host-print-folders-${sanitizer%%,*}"
	TEST_DIR="$(pwd)/$out" timeout 60 "$out/host-print-folders-${sanitizer%%,*}"
done
sh plan/ws309/tests/print-file/run.sh "$out/print-file"
