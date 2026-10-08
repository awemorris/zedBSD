#!/bin/sh
# ws177-p023: builds libkeiland-backend's printers (print.c) with host-print-life.c on the host (ASan and UBSan) and runs
# it against the stand-in daemon fake-printd.py, in a new folder under build/tmp (nothing is removed here; Q1's
# plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-print-life.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
. plan/tools/fresh-out.sh
fresh_out "$repo/build/tmp/ws177-print-life"
work=$fresh_dir
cc -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -I. plan/ws177/tests/host-print-life.c userland/desktop/libkeiland-backend/print/print.c \
	-o "$work/host-print-life"
python3 -c 'import os, sys; open(sys.argv[1], "wb").write(b"%PDF-1.4\n" + os.urandom(10000) + b"\n%%EOF\n")' "$work/doc.pdf"
ASAN_OPTIONS=detect_leaks=0 timeout 180 "$work/host-print-life" "$repo/plan/ws177/tests/fake-printd.py" "$work" "$work/doc.pdf"
