#!/bin/sh
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
#
# Report what an Emacs Lisp file needs that remacs does not have.
#
# Usage: tools/compat-report.sh <file.el> [file.el ...]
#
# Runs from the repository root and needs the development launcher
# (cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug . && cmake --build build-debug),
# because the scanner is a development tool rather than part of a bundle.

set -eu

REMACS=${REMACS:-build-debug/remacs}

if [ $# -eq 0 ]; then
    echo "usage: tools/compat-report.sh <file.el> [file.el ...]" >&2
    exit 1
fi

if [ ! -x "$REMACS" ]; then
    echo "compat-report.sh: no launcher at $REMACS" >&2
    echo "  cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug . && cmake --build build-debug -j" >&2
    exit 1
fi

for f in "$@"; do
    "$REMACS" --compat "$f"
done
