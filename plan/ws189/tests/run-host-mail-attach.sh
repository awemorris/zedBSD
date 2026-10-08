#!/bin/sh
# Builds and runs the host test of Mail's attachments (ws189-p004: userland/desktop/mailer/compose.c, mime.c and what
# mime.c reads with, compiled unchanged) under ASan and UBSan.
# Usage: plan/ws189/tests/run-host-mail-attach.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/ws189-host-mail-attach}
mkdir -p "$out"
M=userland/desktop/mailer
${CC:-cc} -std=gnu99 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -I. \
	plan/ws189/tests/host-mail-attach.c $M/compose.c $M/mime.c $M/jis.c $M/structure.c $M/code.c \
	-o "$out/host-mail-attach"
"$out/host-mail-attach"
