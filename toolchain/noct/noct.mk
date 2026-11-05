# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Noct host toolchain
#

NOCT_TOOLCHAIN_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT          ?= $(abspath $(NOCT_TOOLCHAIN_DIR)/../..)

HOST_NOCT          := $(REPO_ROOT)/build/NoctLang/build-static/noct
export HOST_NOCT
NOCT               ?= $(HOST_NOCT)

NOCT_HOST_SRC      := $(REPO_ROOT)/build/NoctLang
NOCT_HOST_BUILD    := $(NOCT_HOST_SRC)/build-static
NOCT_HOST_BIN      := $(HOST_NOCT)

NOCT_DISTFILE      ?= $(REPO_ROOT)/userland/base/noct/distfiles/NoctLang-f6efa83a8ccb9262ce7f802c0e89e23a39c31c5a.tar.gz
NOCT_PATCHES       := $(sort $(wildcard $(REPO_ROOT)/userland/base/noct/patches/*.patch))
NOCT_PATCH_STAMP   := $(NOCT_HOST_SRC)/.patched

include $(REPO_ROOT)/build-jobs.mk
BUILD_JOBS ?= $(ZEDBSD_BUILD_JOBS)

.PHONY: noct-toolchain noct-toolchain-cached

noct-toolchain: $(NOCT_HOST_BIN)

noct-toolchain-cached: noct-toolchain

$(NOCT_DISTFILE):
	@mkdir -p $(dir $@)
	curl --fail --location --output $@ "https://codeload.github.com/awemorris/NoctLang/tar.gz/f6efa83a8ccb9262ce7f802c0e89e23a39c31c5a"

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
		cmake --preset static -B $(NOCT_HOST_BUILD)
	cd $(NOCT_HOST_SRC) && \
		env -u CFLAGS -u CPPFLAGS -u LDFLAGS \
		cmake --build $(NOCT_HOST_BUILD) --parallel $(BUILD_JOBS)
