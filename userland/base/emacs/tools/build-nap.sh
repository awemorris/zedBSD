#!/bin/sh
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
#
# Bundle the whole editor into a single bytecode file, runnable by an
# unmodified noct with the standard APIs enabled:
#
#     noct remacs.nap [file]
#
# The 8.3 name keeps it usable from FAT16 filesystems.
#
# Usage: tools/build-nb.sh <noct-binary> <generated-dir> <out-dir>

set -eu

NOCT=$1
GEN=$2
OUT=$3

mkdir -p "$OUT"

"$NOCT" --compile --app -O2 $OUT/remacs.nap \
    editor/buffer.noct \
    editor/commands.noct \
    editor/keys.noct \
    editor/lisp.noct \
    editor/lispbuiltins.noct \
    editor/lispcompile.noct \
    "$GEN/lisp-bridge.noct" \
    "$GEN/napi-init.noct" \
    editor/minibuf.noct \
    editor/isearch.noct \
    editor/replace.noct \
    editor/skk.noct \
    editor/shell.noct \
    editor/gud.noct \
    editor/compile.noct \
    editor/window.noct \
    editor/keymap.noct \
    editor/redisplay.noct \
    editor/boot.noct

echo "Built $OUT/remacs.nap"
