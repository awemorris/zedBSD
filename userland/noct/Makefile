# Canonical NoctLang source acquisition and zedBSD target build.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

NOCT_REPOSITORY ?= https://github.com/awemorris/NoctLang.git
NOCT_REVISION ?= eba2043ca74b8601d68a405ecbbeca50ca8d5ac0
NOCT_SOURCE_DIR ?= NoctLang
ZEDBSD_SOURCE_DIR ?= $(abspath ../..)
NOCT_BUILD_JOBS ?= 16

.PHONY: all checkout configure build clean

all: build

$(NOCT_SOURCE_DIR)/.git:
	git clone $(NOCT_REPOSITORY) $(NOCT_SOURCE_DIR)

checkout: $(NOCT_SOURCE_DIR)/.git
	@actual=$$(git -C $(NOCT_SOURCE_DIR) rev-parse HEAD); \
	if test "$$actual" != "$(NOCT_REVISION)" || \
	    git -C $(NOCT_SOURCE_DIR) symbolic-ref -q HEAD >/dev/null; then \
		if test "$$actual" != "$(NOCT_REVISION)"; then \
			git -C $(NOCT_SOURCE_DIR) fetch origin $(NOCT_REVISION); \
		fi; \
		git -C $(NOCT_SOURCE_DIR) checkout --detach $(NOCT_REVISION); \
	fi
	@git -C $(NOCT_SOURCE_DIR) diff --quiet && \
		git -C $(NOCT_SOURCE_DIR) diff --cached --quiet || { \
		echo "NoctLang checkout has local source changes: $(NOCT_SOURCE_DIR)" >&2; \
		exit 1; \
	}

configure: checkout
	cd $(NOCT_SOURCE_DIR) && \
		ZEDBSD_SOURCE_DIR=$(ZEDBSD_SOURCE_DIR) cmake --preset zedbsd

build: configure
	cd $(NOCT_SOURCE_DIR) && \
		ZEDBSD_SOURCE_DIR=$(ZEDBSD_SOURCE_DIR) \
		cmake --build --preset zedbsd --parallel $(NOCT_BUILD_JOBS)
	@test -x $(NOCT_SOURCE_DIR)/build-zedbsd/noct

clean:
	@if test -f $(NOCT_SOURCE_DIR)/build-zedbsd/CMakeCache.txt; then \
		cmake --build $(NOCT_SOURCE_DIR)/build-zedbsd --target clean; \
	fi
