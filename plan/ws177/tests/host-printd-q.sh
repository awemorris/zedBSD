#!/bin/sh
# ws177-p022: builds keiland-printd on the host twice (with ASan and UBSan, and plain for the preloaded slow resolver)
# and runs host-printd-q.py against mock-printers-q.py, in a new folder under build/tmp (nothing is removed here; Q1's
# plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-printd-q.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
. plan/tools/fresh-out.sh
fresh_out "$repo/build/tmp/ws177-printd-q"
work=$fresh_dir
flags='-std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread -Iuserland/desktop/printd'
cc $flags -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer userland/desktop/printd/*.c \
	-o "$work/keiland-printd"
cc $flags userland/desktop/printd/*.c -o "$work/keiland-printd-plain"
cc -std=gnu99 -O1 -Wall -Wextra -Werror -fPIC -shared plan/ws177/tests/slow-resolver.c -ldl -o "$work/slow-resolver.so"
timeout 300 python3 plan/ws177/tests/host-printd-q.py "$work/keiland-printd" "$work/keiland-printd-plain" \
	"$work/slow-resolver.so" "$work"
