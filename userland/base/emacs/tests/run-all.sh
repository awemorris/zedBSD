#!/bin/sh
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

set -eu

REMACS=${REMACS:-../build-debug/remacs}

echo '=== remacs test suites ==='
sh run-unit.sh
echo
sh run-lisp.sh
echo
sh run-lisp-tail.sh
echo
echo 'pty tests'
python3 term/harness.py "$REMACS" $(ls term/case_*.py | grep -v case_nb)
echo
echo 'bytecode bundle test (plain noct)'
python3 term/harness.py ../build-debug/noctlang/noct term/case_nb.py
echo
sh run-oracle.sh
echo
echo 'All remacs tests passed.'
