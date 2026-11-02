#!/bin/sh
# Builds and runs the host test of the compositor's "KWL PRESS" lines (seat.c linked with
# stand-ins, host-press-log.c), plain and under ASan/UBSan.  Last line: host-press-log: PASS.
#   sh plan/bugs/BUG-412/host-press-log.sh [build-dir]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
cd "$(dirname -- "$0")/../../.."
out=${1:-build/bug412-host}
mkdir -p "$out"
for flavour in plain sanitize; do
	flags=""
	if [ "$flavour" = sanitize ]; then
		flags="-fsanitize=address,undefined -fno-sanitize-recover=all"
	fi
	${CC:-cc} -std=gnu89 -Wall -Wextra -Werror -g -I. $flags \
	    userland/desktop/wayland/seat.c plan/bugs/BUG-412/host-press-log.c -o "$out/host-press-log-$flavour"
	timeout 60 "$out/host-press-log-$flavour" "$out/press-$flavour.log"
done
echo "host-press-log: PASS (plain, ASan/UBSan)"
