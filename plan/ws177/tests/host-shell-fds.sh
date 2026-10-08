#!/bin/sh
# ws177-p019: builds and runs the host test of the browser shell's network descriptors (host-shell-fds.c with
# userland/desktop/browser/shell/window.c and fakes of libkeiland's application) under ASan and UBSan, in a new
# directory under build/tmp (nothing is removed here; Q1's plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-shell-fds.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws177-shell-fds"
cd "$repo"
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	-Iuserland/desktop/browser -Iuserland/desktop/include -I. plan/ws177/tests/host-shell-fds.c \
	userland/desktop/browser/shell/window.c -o "$fresh_dir/host-shell-fds"
timeout 20 "$fresh_dir/host-shell-fds"
