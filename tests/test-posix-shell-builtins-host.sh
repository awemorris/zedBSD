#!/usr/bin/env bash
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -euo pipefail

# POSIX-UTILITY-TEST: hash positive negative
# POSIX-UTILITY-TEST: ulimit positive negative

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/zedbsd-shell-builtins.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
	-I"$repo" -I"$repo/include/uapi" \
	"$repo/userland/base/sh/builtins.c" \
	"$repo/tests/sh-posix-builtins-host-test.c" -o "$work/test"
"$work/test"
