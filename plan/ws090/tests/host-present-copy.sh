#!/bin/sh
# BUG-221: builds and runs the host test of the presenter's copy by a changed part (host-present-copy.c over
# userland/desktop/libkeiland/ui/present-copy.c), with ASan and UBSan.
#   sh plan/ws090/tests/host-present-copy.sh [OUT]   (default build/ws090-host/host-present-copy)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -e
cd "$(dirname "$0")/../../.."
out=${1:-build/ws090-host/host-present-copy}
mkdir -p "$(dirname "$out")"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I. -Iuserland/desktop/include \
	userland/desktop/libkeiland/ui/present-copy.c plan/ws090/tests/host-present-copy.c -o "$out"
"$out"
