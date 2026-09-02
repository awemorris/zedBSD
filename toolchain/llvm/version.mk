# Canonical LLVM release identity for the project-owned x86 toolchain.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

override ZEDBSD_LLVM_VERSION := 23.1.0
override ZEDBSD_LLVM_TAG := llvmorg-23.1.0
override ZEDBSD_LLVM_ARCHIVE_ROOT := llvm-project-23.1.0.src
override ZEDBSD_LLVM_ARCHIVE_NAME := llvm-project-23.1.0.src.tar.xz
override ZEDBSD_LLVM_ARCHIVE_URL := https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.0/llvm-project-23.1.0.src.tar.xz
override ZEDBSD_LLVM_ARCHIVE_SIZE := 179140728
override ZEDBSD_LLVM_ARCHIVE_SHA256 := ab1f0e3ec52448c33e8782eaf0422504b87c7b016b22514653ee0d8fcee479ff
override ZEDBSD_LLVM_PATCH_LEVEL := zedbsd3
override ZEDBSD_LLVM_CACHE_TAG := rev-0
override ZEDBSD_LLVM_CACHE_ASSET := zedbsd-llvm-23.1.0-x86_64-linux.tar.gz
# Filled from the accepted deterministic archive before the release asset is
# uploaded. A cache download must never run with this sentinel value.
override ZEDBSD_LLVM_CACHE_SHA256 := 6f8e1154c73b9f2d32f16360ace107b7862f08e748c6f10c1bd75914aa6502c2
