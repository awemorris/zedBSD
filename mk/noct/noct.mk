# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Noct host toolchain
#

NOCT_TOOLCHAIN_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT          ?= $(abspath $(NOCT_TOOLCHAIN_DIR)/../..)

HOST_NOCT          := $(REPO_ROOT)/build/NoctLang/build/noct
export HOST_NOCT
NOCT               ?= $(HOST_NOCT)

NOCT_HOST_SRC      := $(REPO_ROOT)/build/NoctLang
NOCT_HOST_BUILD    := $(NOCT_HOST_SRC)/build
NOCT_HOST_BIN      := $(HOST_NOCT)

NOCT_DISTFILE      ?= $(REPO_ROOT)/userland/base/noct/distfiles/NoctLang-d198b952dec90120c3d0e7987dd12af06911478e.tar.gz
NOCT_PATCHES       := $(sort $(wildcard $(REPO_ROOT)/userland/base/noct/patches/*.patch))
NOCT_PATCH_STAMP   := $(NOCT_HOST_SRC)/.patched

include $(REPO_ROOT)/build-jobs.mk
BUILD_JOBS ?= $(ZEDBSD_BUILD_JOBS)

.PHONY: noct-toolchain noct-toolchain-cached

noct-toolchain: $(NOCT_HOST_BIN)

noct-toolchain-cached: noct-toolchain

$(NOCT_DISTFILE):
	@mkdir -p $(dir $@)
	curl --fail --location --output $@ "https://codeload.github.com/awemorris/NoctLang/tar.gz/d198b952dec90120c3d0e7987dd12af06911478e"

$(NOCT_PATCH_STAMP): $(NOCT_DISTFILE) $(NOCT_PATCHES)
	@mkdir -p $(NOCT_HOST_SRC)
	@if [ ! -f $(NOCT_HOST_SRC)/CMakeLists.txt ]; then \
		tar -xzf $(NOCT_DISTFILE) --strip-components=1 -C $(NOCT_HOST_SRC); \
		for p in $(NOCT_PATCHES); do \
			patch -d $(NOCT_HOST_SRC) -p1 < "$$p" || exit 1; \
		done; \
	fi
	@touch $@

$(NOCT_HOST_BIN): $(NOCT_PATCH_STAMP)
	@mkdir -p $(NOCT_HOST_BUILD)
	cd $(NOCT_HOST_SRC) && \
		env -u CFLAGS -u CPPFLAGS -u LDFLAGS \
		CC="$(HOST_CC)" CXX="$(HOST_CXX)" \
		cmake -B $(NOCT_HOST_BUILD) \
			-DCMAKE_BUILD_TYPE=Release \
			-DNOCT_ENABLE_INSTALL=ON \
			-DNOCT_ENABLE_STATIC=ON \
			-DNOCT_ENABLE_CLI=ON \
			-DNOCT_ENABLE_REPL=ON \
			-DNOCT_ENABLE_JIT=ON \
			-DNOCT_ENABLE_MULTITHREAD=ON \
			-DNOCT_ENABLE_OPTIMIZER=ON \
			-DNOCT_ENABLE_API=ON \
			-DNOCT_ENABLE_API_SYSTEM=ON \
			-DNOCT_ENABLE_API_CONSOLE=ON \
			-DNOCT_ENABLE_API_FILE=ON \
			-DNOCT_ENABLE_API_THREAD=ON \
			-DNOCT_ENABLE_API_TERM=ON \
			-DNOCT_ENABLE_API_PROCESS=ON \
			-DNOCT_ENABLE_API_HTTPSERVER=ON \
			-DNOCT_ENABLE_BCBACKEND=ON
	cd $(NOCT_HOST_SRC) && \
		env -u CFLAGS -u CPPFLAGS -u LDFLAGS \
		cmake --build $(NOCT_HOST_BUILD) --parallel $(BUILD_JOBS)
	@ln -sfn build $(NOCT_HOST_SRC)/build-static
	@mkdir -p $(REPO_ROOT)/build/host-noct-state
	@touch $(REPO_ROOT)/build/host-noct-state/built-d198b952dec90120c3d0e7987dd12af06911478e
	@touch $@
