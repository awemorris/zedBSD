# Canonical Noct release identity shared by host and target builds.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

override ZEDBSD_NOCT_VERSION := 2.0.3
# Upstream release v2.0.3 (tag v2.0.3 is this commit): the earlier post-2.0.1
# snapshot plus the interpreter's missing-return fix. It carries the terminal
# and BeUI work that used to be local patches, so only the cross-build patches
# remain.
override ZEDBSD_NOCT_TAG := f6efa83a8ccb9262ce7f802c0e89e23a39c31c5a
override ZEDBSD_NOCT_TAG_COMMIT := f6efa83a8ccb9262ce7f802c0e89e23a39c31c5a
override ZEDBSD_NOCT_ARCHIVE_ROOT := NoctLang-f6efa83a8ccb9262ce7f802c0e89e23a39c31c5a
override ZEDBSD_NOCT_ARCHIVE_NAME := NoctLang-f6efa83a8ccb9262ce7f802c0e89e23a39c31c5a.tar.gz
override ZEDBSD_NOCT_ARCHIVE_URL := https://codeload.github.com/awemorris/NoctLang/tar.gz/f6efa83a8ccb9262ce7f802c0e89e23a39c31c5a
override ZEDBSD_NOCT_ARCHIVE_SIZE := 2529835
override ZEDBSD_NOCT_ARCHIVE_SHA256 := b46863cecfc885a98d3d84e8ff892d0205408e47cf08be024f5e0d502ac0006c
override ZEDBSD_NOCT_PATCH_LEVEL := zedbsd12
# ws101-p011: the zedBSD interpreter's source tree carries target-only patches
# (ZEDBSD_NOCT_TARGET_PATCHES in Makefile) on top of the ones above and is
# stamped with this level of its own, so that a change of a target-only patch
# leaves the host interpreter (build/NoctLang, shared by every checkout) as it
# is.  Raise the -tN suffix when a target-only patch changes.
override ZEDBSD_NOCT_TARGET_PATCH_LEVEL := $(ZEDBSD_NOCT_PATCH_LEVEL)-t1
