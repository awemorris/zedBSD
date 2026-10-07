#!/bin/sh
# ws177-p008: builds and runs the host test of the recent list's stamp (host-recent-stamp.c with
# userland/desktop/libkeiland/recent.c) under ASan and UBSan, with a data folder of its own in a new directory under
# build/tmp (nothing is removed here; Q1's plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-recent-stamp.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws177-recent-stamp"
work=$fresh_dir
mkdir "$work/data"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$repo/userland/desktop/include" "$repo/plan/ws177/tests/host-recent-stamp.c" "$repo/userland/desktop/libkeiland/recent.c" \
	-o "$work/stamp"
XDG_DATA_HOME="$work/data" timeout 20 "$work/stamp"
