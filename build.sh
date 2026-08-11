#!/usr/bin/env bash
# zedBSD build driver: ./build.sh <arch> [make targets and options...]
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
#
# Examples:
#   ./build.sh pc98              build every pc98 artifact
#   ./build.sh pc98 check        build and run the host test suite
#   ./build.sh pc98 clean        remove build/pc98
set -euo pipefail

repo="$(cd "$(dirname "$0")" && pwd)"

available() {
	local mk
	for mk in "$repo"/platform/*/platform.mk; do
		test -f "$mk" || continue
		basename "$(dirname "$mk")"
	done
}

if test "$#" -lt 1; then
	echo "usage: $0 <arch> [make targets and options...]" >&2
	echo "available architectures:" >&2
	available | sed 's/^/  /' >&2
	exit 2
fi

arch="$1"
shift

if ! test -f "$repo/platform/$arch/platform.mk"; then
	echo "unknown architecture: $arch" >&2
	echo "available architectures:" >&2
	available | sed 's/^/  /' >&2
	exit 2
fi

jobs="${ZEDBSD_JOBS:-$(nproc)}"
exec make -C "$repo" ARCH="$arch" -j"$jobs" "$@"
