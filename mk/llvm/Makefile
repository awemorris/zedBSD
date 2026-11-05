# Standalone entry point for the project-owned LLVM toolchain.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_REPO_ROOT := $(abspath ../..)
include $(ZEDBSD_REPO_ROOT)/toolchain/llvm/llvm.mk

.DEFAULT_GOAL := build

.PHONY: download patch build install
download: llvm-download
patch: llvm-source-verify
build: llvm-build
install: llvm-toolchain
