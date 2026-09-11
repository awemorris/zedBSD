# Canonical Noct release identity shared by host and target builds.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

override ZEDBSD_NOCT_VERSION := 2.0.1
# This post-2.0.1 snapshot retains the upstream version; the identity ref is a
# commit on main. It carries the terminal and BeUI work that used to be local
# patches, so only the cross-build patches remain.
override ZEDBSD_NOCT_TAG := fcf5759eddfac9c36d6eb88323d62da6148b3822
override ZEDBSD_NOCT_TAG_COMMIT := fcf5759eddfac9c36d6eb88323d62da6148b3822
override ZEDBSD_NOCT_ARCHIVE_ROOT := NoctLang-fcf5759eddfac9c36d6eb88323d62da6148b3822
override ZEDBSD_NOCT_ARCHIVE_NAME := NoctLang-fcf5759eddfac9c36d6eb88323d62da6148b3822.tar.gz
override ZEDBSD_NOCT_ARCHIVE_URL := https://codeload.github.com/awemorris/NoctLang/tar.gz/fcf5759eddfac9c36d6eb88323d62da6148b3822
override ZEDBSD_NOCT_ARCHIVE_SIZE := 2529920
override ZEDBSD_NOCT_ARCHIVE_SHA256 := 0083328ee970c715619bad15d6b1fea60b19e5764522273b233142e7add53d21
override ZEDBSD_NOCT_PATCH_LEVEL := zedbsd11
