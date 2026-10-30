#!/bin/sh
# What the implementation phases may not change is as the design left it: every file listed in frozen.sha256 beside
# this script (the public headers, libkeiland's exports and POSIX layers and Makefiles, the design and the tests'
# tools) has its recorded SHA-256, and the branch's changes since it left main have no whitespace errors.  Run from the worktree's root.
#   sh plan/ws327/tests/check-base.sh          (make-frozen.sh, the design's, writes frozen.sha256)
# Prints the files that differ and "ws327-check-base: PASS" or "FAIL".
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
failed=0
if ! sha256sum --quiet -c plan/ws327/tests/frozen.sha256; then
	echo "ws327-check-base: a frozen file changed (above); stop and report it instead of changing it"
	failed=1
fi
# The changes since the branch left main, committed or not.
fork=$(git merge-base HEAD main)
if ! git diff --check "$fork" > /dev/null 2>&1; then
	git diff --check "$fork"
	echo "ws327-check-base: whitespace errors (above)"
	failed=1
fi
[ $failed = 0 ] && echo "ws327-check-base: PASS" || echo "ws327-check-base: FAIL"
[ $failed = 0 ]
