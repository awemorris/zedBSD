#!/bin/sh
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

set -eu

REMACS=${REMACS:-../build-debug/remacs}

echo 'remacs unit tests'
echo

for tc in unit/*.noct; do
    echo "$tc"
    $REMACS --script "$tc" > out || true
    diff "$tc.out" out
done
rm -f out
echo 'All unit tests passed.'
