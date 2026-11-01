#!/bin/sh
# The host test of sessiond's rules for a session that crashed (fallback-rules.c): what a crash is, and when the same
# user's session starts again without the login screen or the login screen comes (the third crash in two minutes).
# Under ASan and UBSan.  The last line is "restart-rules-host-test: PASS".
#   sh plan/tools/sessiond/restart-rules-host-test.sh   (from the repository's top)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
OUT=$(realpath "$(mktemp -d build/tmp/sessiond-restart.XXXXXX)")
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I. \
	-o "$OUT/restart-rules-host-test" plan/tools/sessiond/restart-rules-host-test.c userland/desktop/sessiond/fallback-rules.c
timeout 60 "$OUT/restart-rules-host-test"
