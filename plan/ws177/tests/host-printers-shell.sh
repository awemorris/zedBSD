#!/bin/sh
# ws177-p024: builds the compositor's printers object (printers-shell.c) with libkeiland-backend's print.c and
# host-printers-shell.c on the host (ASan and UBSan), its daemon the stand-in fake-printd.py, and runs it; then the
# source check that every object kind reaches its handler through protocol.c (host-dispatch-kinds.py).  In a new
# folder under build/tmp (nothing is removed here; Q1's plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-printers-shell.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
. plan/tools/fresh-out.sh
fresh_out "$repo/build/tmp/ws177-printers-shell"
work=$fresh_dir
mkdir -p "$work/include/keiland" "$work/libexec" "$work/fake" "$work/home" "$work/runtime"
chmod 700 "$work/runtime"
cp userland/desktop/include/keiland/keiland.h "$work/include/keiland/keiland.h"
printf 'normal\n' > "$work/fake/modes"
printf '#!/bin/sh\nexec python3 %s %s\n' "$repo/plan/ws177/tests/fake-printd.py" "$work/fake" > "$work/libexec/keiland-printd"
chmod 700 "$work/libexec/keiland-printd"
python3 -c 'import os, sys; open(sys.argv[1], "wb").write(b"%PDF-1.4\n" + os.urandom(10000) + b"\n%%EOF\n")' "$work/doc.pdf"
cc -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -I. -I"$work/include" -DKEILAND_LIBEXECDIR="\"$work/libexec\"" \
	plan/ws177/tests/host-printers-shell.c userland/desktop/wayland/printers-shell.c \
	userland/desktop/libkeiland-backend/print/print.c -o "$work/host-printers-shell"
XDG_RUNTIME_DIR="$work/runtime" ASAN_OPTIONS=detect_leaks=0 timeout 120 "$work/host-printers-shell" "$work/home" "$work/doc.pdf"
python3 plan/ws177/tests/host-dispatch-kinds.py "$repo"
