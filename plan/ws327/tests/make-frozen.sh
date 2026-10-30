#!/bin/sh
# Writes frozen.sha256 (check-base.sh): the SHA-256 of every file the implementation phases may not change.  The
# design phase runs it as its last step; nobody else does.  Run from the worktree's root.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
git ls-files userland/desktop/include/keiland/ userland/desktop/libkeiland/exports.map userland/desktop/libkeiland/exports.py \
	userland/desktop/libkeiland/platform/thread-posix.c userland/desktop/libkeiland/platform/socket-posix.c \
	userland/desktop/libkeiland/Makefile userland/desktop/libkeiland/Makefile.linux userland/desktop/libkeiland/Makefile.freebsd \
	plan/ws327/design.md plan/ws327/tests/ |
	grep -v '^plan/ws327/tests/frozen\.sha256$' | xargs sha256sum > plan/ws327/tests/frozen.sha256
wc -l < plan/ws327/tests/frozen.sha256
