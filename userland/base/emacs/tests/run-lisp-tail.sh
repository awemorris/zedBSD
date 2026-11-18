#!/bin/sh
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
#
# Deep proper-tail-call tests. These intentionally exceed GNU Emacs's normal
# evaluation depth, so unlike run-lisp.sh they are REmacs-only tests.

set -eu

REMACS=${REMACS:-../build-debug/remacs}
STEPS=${REMACS_TAIL_STEPS:-100000}

echo "remacs Lisp deep tail-call tests ($STEPS steps)"
mkdir -p lisp-tail/build
fails=0

for tc in lisp-tail/*.el; do
    name=$(basename "$tc" .el)
    case_fails=0
    for compile in 0 1; do
        mode=interpreter
        if [ "$compile" -eq 1 ]; then mode=compiler; fi
        wrapper="lisp-tail/build/$name-$mode.noct"
        log="lisp-tail/build/$name-$mode.log"
        cat > "$wrapper" <<NEOF
func main() {
    remacsInitCore();
    LispCompileEnabled = $compile;
    var text = "(setq tail-steps $STEPS)\\n" + FileUtil.readText("$tc");
    if (lispEvalText(text) == 0) {
        print("LISP ERROR: " + LispErrMsg);
    }
    return 0;
}
NEOF
        if ! "$REMACS" --script "$wrapper" > "$log"; then
            echo "FAIL $tc ($mode): remacs exited unsuccessfully"
            fails=$((fails+1))
            case_fails=$((case_fails+1))
            continue
        fi
        if ! diff "${tc%.el}.expected" "$log" > "lisp-tail/build/$name-$mode.diff"; then
            echo "FAIL $tc ($mode)"
            head -10 "lisp-tail/build/$name-$mode.diff"
            fails=$((fails+1))
            case_fails=$((case_fails+1))
        fi
    done
    if ! diff "lisp-tail/build/$name-interpreter.log" \
              "lisp-tail/build/$name-compiler.log" \
              > "lisp-tail/build/$name-modes.diff"; then
        echo "MODE MISMATCH $tc"
        head -10 "lisp-tail/build/$name-modes.diff"
        fails=$((fails+1))
        case_fails=$((case_fails+1))
    elif [ "$case_fails" -eq 0 ]; then
        echo "PASS $tc"
    fi
done

# Prove that the mixed case really crossed the compiler/interpreter boundary;
# output equivalence alone would also pass if both defuns stayed interpreted.
mixed_check=lisp-tail/build/05-mixed-dispatch.noct
mixed_log=lisp-tail/build/05-mixed-dispatch.log
cat > "$mixed_check" <<NEOF
func main() {
    remacsInitCore();
    LispCompileEnabled = 1;
    var text = "(setq tail-steps 1000)\\n" +
               FileUtil.readText("lisp-tail/05-mixed-compiled.el");
    if (lispEvalText(text) == 0) {
        print("LISP ERROR: " + LispErrMsg);
        return 0;
    }
    if (Dict.hasKey(LispCompiled, "deep-tail-compiled") == 1 &&
        Dict.hasKey(LispCompiled, "deep-tail-interpreted") == 0) {
        print("PASS mixed-dispatch");
    } else {
        print("FAIL mixed-dispatch");
    }
    return 0;
}
NEOF
if ! "$REMACS" --script "$mixed_check" > "$mixed_log" ||
   ! grep -q '^PASS mixed-dispatch$' "$mixed_log"; then
    echo 'FAIL compiled/interpreted dispatch proof'
    cat "$mixed_log"
    fails=$((fails+1))
else
    echo 'PASS compiled/interpreted dispatch proof'
fi

[ "$fails" -eq 0 ] && echo 'All deep tail-call tests passed.' || exit 1
