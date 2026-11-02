#!/bin/sh
# ws345: builds and runs plan/ws345/tests/host-prompt-rule.c (the rule of the browser shell's question about a page that does
# not respond, userland/desktop/browser/shell/unresponsive.c, alone: it needs nothing of the window) with the host's C compiler.
#
#   sh plan/ws345/tests/host-prompt-rule.sh [OUT]     (OUT default build/tmp/host-prompt-rule, made afresh)
#
# Last line: the test's summary; the exit status is the test's.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
. plan/tools/fresh-out.sh
out=${1:-build/tmp/host-prompt-rule}
fresh_out "$out"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -O1 -Iuserland/desktop/browser -o "$out/host-prompt-rule" \
	plan/ws345/tests/host-prompt-rule.c userland/desktop/browser/shell/unresponsive.c
timeout 60 "$out/host-prompt-rule"
