# Standalone entry point for the project-owned LLVM toolchain.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_REPO_ROOT := $(abspath ../..)
include $(ZEDBSD_REPO_ROOT)/mk/llvm/llvm.mk

.DEFAULT_GOAL := build

.PHONY: download patch build install
download: llvm-download
patch: llvm-source
build: llvm-build
install: llvm-toolchain
