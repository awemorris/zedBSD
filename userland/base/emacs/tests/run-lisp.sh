#!/bin/sh
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
#
# Lisp oracle: each tests/lisp/*.el runs in the remacs Lisp
# interpreter and in real GNU Emacs; the princ streams must match.

set -u

REMACS=${REMACS:-../build-debug/remacs}

echo 'remacs Lisp oracle tests'
mkdir -p lisp/build
fails=0
for tc in lisp/*.el; do
    name=$(basename "$tc" .el)

    # Side A: the remacs Lisp interpreter.
    cat > "lisp/build/$name-vm.noct" <<NEOF
func main() {
    remacsInitCore();
    if (lispLoadFile("$tc") == 0) {
        print("LISP ERROR: " + LispErrMsg);
    }
    return 0;
}
NEOF
    $REMACS --script "lisp/build/$name-vm.noct" > "lisp/build/$name-vm.log"

    # Differential: the Lisp compiler (on by default) must produce the
    # same output as the pure interpreter.
    cat > "lisp/build/$name-nc.noct" <<NEOF
func main() {
    remacsInitCore();
    LispCompileEnabled = 0;
    if (lispLoadFile("$tc") == 0) {
        print("LISP ERROR: " + LispErrMsg);
    }
    return 0;
}
NEOF
    $REMACS --script "lisp/build/$name-nc.noct" > "lisp/build/$name-nc.log"
    if ! diff "lisp/build/$name-vm.log" "lisp/build/$name-nc.log" > "lisp/build/$name-nc.diff"; then
        echo "COMPILER MISMATCH $tc"
        head -6 "lisp/build/$name-nc.diff"
        fails=$((fails+1))
    fi

    # Side B: real Emacs.
    emacs -Q --batch --eval "(with-current-buffer (get-buffer-create \"*scratch*\") (load (expand-file-name \"$tc\") nil t))" \
        > "lisp/build/$name-emacs.log" 2>/dev/null

    if diff "lisp/build/$name-vm.log" "lisp/build/$name-emacs.log" > "lisp/build/$name.diff"; then
        echo "PASS $tc"
    else
        echo "FAIL $tc"
        head -10 "lisp/build/$name.diff"
        fails=$((fails+1))
    fi
done
[ $fails -eq 0 ] && echo 'All Lisp tests passed.' || exit 1
