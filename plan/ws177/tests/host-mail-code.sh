#!/bin/sh
# ws177-p014: builds and runs the host test of a message's sign-in code (host-mail-code.c with
# userland/desktop/mailer/code.c) under ASan and UBSan, in a new directory under build/tmp (nothing is removed here;
# Q1's plan/tools/q1-clean.sh removes old runs).
#   sh plan/ws177/tests/host-mail-code.sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws177-mail-code"
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$repo" "$repo/plan/ws177/tests/host-mail-code.c" "$repo/userland/desktop/mailer/code.c" -o "$fresh_dir/code"
timeout 20 "$fresh_dir/code"
