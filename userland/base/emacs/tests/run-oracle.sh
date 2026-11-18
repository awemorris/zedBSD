#!/bin/sh
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
#
# The oracle harness: run each test on the remacs VM and, transpiled,
# inside real GNU Emacs, then diff the two observation logs. No
# expected output is stored anywhere.

set -eu

REMACS=${REMACS:-../build-debug/remacs}
NOCT=${NOCT:-../build-debug/noctlang/noct}
GEN=${GEN:-../build-debug/generated}

echo 'remacs oracle tests (GNU Emacs as the oracle)'
emacs --version | head -1
echo

mkdir -p oracle/build
fails=0
for tc in oracle/[0-9]*.noct; do
    name=$(basename "$tc" .noct)

    # Side A: the remacs VM.
    cat oracle/prelude.noct "$tc" > "oracle/build/$name-vm.noct"
    $REMACS --script "oracle/build/$name-vm.noct" > "oracle/build/$name-vm.log"

    # Side B: real Emacs.
    $NOCT --elisp "oracle/build/$name.el" "--ns-map=$GEN/elisp-ns-map.txt" "$tc" >/dev/null
    emacs -Q --batch -l "$GEN/noct-shim.el" -l "oracle/build/$name.el" \
        --eval '(with-current-buffer (get-buffer-create "*scratch*") (buffer-enable-undo) (oracleTest))' > "oracle/build/$name-emacs.log"

    if diff "oracle/build/$name-vm.log" "oracle/build/$name-emacs.log" > "oracle/build/$name.diff"; then
        echo "PASS $tc"
    else
        echo "FAIL $tc"
        cat "oracle/build/$name.diff" | head -10
        fails=$((fails+1))
    fi
done

[ $fails -eq 0 ] && echo 'All oracle tests passed.' || exit 1
