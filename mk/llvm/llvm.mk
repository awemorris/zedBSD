# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# LLVM toolchain configuration and build
#

TARGET_LLVM_BIN := $(abspath build/llvm/bin)
ZEDBSD_TARGET_LLVM_BIN := $(TARGET_LLVM_BIN)

# Target Triple
ifeq ($(ARCHITECTURE),x86_64)
TARGET_TRIPLE  := x86_64-unknown-zedbsd
else ifeq ($(ARCHITECTURE),i386)
TARGET_TRIPLE  := i386-unknown-zedbsd
else ifeq ($(ARCHITECTURE),arm64)
TARGET_TRIPLE  := aarch64-unknown-zedbsd
else ifeq ($(ARCHITECTURE),armv7)
TARGET_TRIPLE  := armv7-unknown-zedbsd
else ifeq ($(ARCHITECTURE),riscv64)
TARGET_TRIPLE  := riscv64-unknown-zedbsd
else ifeq ($(ARCHITECTURE),riscv32)
TARGET_TRIPLE  := riscv32-unknown-zedbsd
else ifeq ($(ARCHITECTURE),mips64)
TARGET_TRIPLE  := mips64-unknown-zedbsd
else ifeq ($(ARCHITECTURE),mips32)
TARGET_TRIPLE  := mips-unknown-zedbsd
endif

TARGET_SYSROOT := $(abspath $(or $(BUILD),build/$(PLATFORM))/sysroot)

# Toolchain Commands
TARGET_CLANG := $(TARGET_LLVM_BIN)/clang
CC      := $(TARGET_CLANG)
CXX     := $(TARGET_LLVM_BIN)/clang++
AS      := $(CC)
LD      := $(TARGET_LLVM_BIN)/ld.lld
AR      := $(TARGET_LLVM_BIN)/llvm-ar
RANLIB  := $(TARGET_LLVM_BIN)/llvm-ranlib
NM      := $(TARGET_LLVM_BIN)/llvm-nm
OBJCOPY := $(TARGET_LLVM_BIN)/llvm-objcopy
OBJDUMP := $(TARGET_LLVM_BIN)/llvm-objdump
READELF := $(TARGET_LLVM_BIN)/llvm-readelf
STRIP   := $(TARGET_LLVM_BIN)/llvm-strip
CFLAGS  := --target=$(TARGET_TRIPLE) --sysroot=$(TARGET_SYSROOT)

# Kernel LTO
ifeq ($(KERNEL_LTO),full)
KERNEL_LTO_CFLAGS := -flto
else ifeq ($(KERNEL_LTO),thin)
KERNEL_LTO_CFLAGS := -flto=thin
else ifeq ($(KERNEL_LTO),none)
KERNEL_LTO_CFLAGS :=
else
$(error KERNEL_LTO must be full, thin or none, not '$(KERNEL_LTO)')
endif

KERNEL_LTO_STAMP := $(BUILD)/.kernel-lto
.PHONY: FORCE_KERNEL_LTO
FORCE_KERNEL_LTO:
$(KERNEL_LTO_STAMP): FORCE_KERNEL_LTO
	@mkdir -p $(dir $@)
	@value='lto=$(KERNEL_LTO_CFLAGS)'; \
	if ! test -f $@ || ! grep -Fqx -- "$$value" $@; then \
		printf '%s\n' "$$value" > $@.tmp; \
		mv $@.tmp $@; \
	fi

# LLVM Source and Cache Definitions
LLVM_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
include $(LLVM_DIR)/version.mk

REPO_ROOT ?= $(abspath $(LLVM_DIR)/../..)
include $(REPO_ROOT)/mk/build-jobs.mk

LLVM_DISTDIR := $(LLVM_DIR)/distfiles
LLVM_DISTFILE := $(LLVM_DISTDIR)/$(ZEDBSD_LLVM_ARCHIVE_NAME)
LLVM_PATCH := $(LLVM_DIR)/patches/0001-add-zedbsd-x86-target.patch
LLVM_SOURCE := $(REPO_ROOT)/build/llvm-source
LLVM_BUILD := $(REPO_ROOT)/build/llvm-build
LLVM_INSTALL := $(REPO_ROOT)/build/llvm

LLVM_CACHE_URL := https://github.com/awemorris/zedBSD/releases/download/$(ZEDBSD_LLVM_CACHE_TAG)/$(ZEDBSD_LLVM_CACHE_ASSET)
LLVM_CACHE_ARCHIVE := $(REPO_ROOT)/build/releases/$(ZEDBSD_LLVM_CACHE_ASSET)

LLVM_DISTRIBUTION_COMPONENTS := clang;clang-resource-headers;lld;llvm-ar;llvm-ranlib;llvm-nm;llvm-objcopy;llvm-objdump;llvm-readelf;llvm-strip
LLVM_INSTALLED_TOOL_NAMES := clang clang++ ld.lld lld-link llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-objdump llvm-readelf llvm-strip
LLVM_INSTALLED_TOOLS := $(addprefix $(LLVM_INSTALL)/bin/,$(LLVM_INSTALLED_TOOL_NAMES))

.PHONY: llvm-toolchain llvm-toolchain-cached toolchain-cache llvm-download llvm-source

llvm-toolchain: $(LLVM_INSTALLED_TOOLS)
	@'$(LLVM_INSTALL)/bin/clang' --version | grep -F 'clang version $(ZEDBSD_LLVM_VERSION)' >/dev/null
	@'$(LLVM_INSTALL)/bin/ld.lld' --version | grep -F 'LLD $(ZEDBSD_LLVM_VERSION)' >/dev/null

llvm-toolchain-cached:
	@if [ -x "$(LLVM_INSTALL)/bin/clang" ]; then \
		echo "LLVM toolchain already installed at $(LLVM_INSTALL)"; \
	else \
		mkdir -p $(dir $(LLVM_CACHE_ARCHIVE)) $(LLVM_INSTALL); \
		if [ ! -f "$(LLVM_CACHE_ARCHIVE)" ]; then \
			echo "Downloading LLVM cache $(ZEDBSD_LLVM_CACHE_ASSET)..."; \
			curl --fail --location --output "$(LLVM_CACHE_ARCHIVE)" "$(LLVM_CACHE_URL)"; \
		fi; \
		echo "Extracting LLVM cache..."; \
		tar -xzf "$(LLVM_CACHE_ARCHIVE)" -C $(dir $(LLVM_INSTALL)); \
	fi

llvm-download: $(LLVM_DISTFILE)

$(LLVM_DISTFILE):
	@mkdir -p $(dir $@)
	curl --fail --location --output $@ "$(ZEDBSD_LLVM_ARCHIVE_URL)"
	@echo '$(ZEDBSD_LLVM_ARCHIVE_SHA256)  $@' | sha256sum -c -

llvm-source: $(LLVM_SOURCE)/.patched

# The extracted source is named by the archive's hash and the patch's hash,
# not by file times: a source that matches is used as it is, and the archive
# is fetched only when the source has to be made again.  A new source is
# extracted and patched beside the old one and then takes its place.
LLVM_SOURCE_ID := $(ZEDBSD_LLVM_ARCHIVE_SHA256) $(firstword $(shell sha256sum $(LLVM_PATCH)))
ifneq ($(shell cat $(LLVM_SOURCE)/.patched 2>/dev/null),$(LLVM_SOURCE_ID))
$(LLVM_SOURCE)/.patched: llvm-source-stale
endif
.PHONY: llvm-source-stale
llvm-source-stale:

$(LLVM_SOURCE)/.patched:
	@if [ ! -f $(LLVM_DISTFILE) ]; then \
		mkdir -p $(LLVM_DISTDIR); \
		curl --fail --location --output $(LLVM_DISTFILE) "$(ZEDBSD_LLVM_ARCHIVE_URL)"; \
	fi
	@echo '$(ZEDBSD_LLVM_ARCHIVE_SHA256)  $(LLVM_DISTFILE)' | sha256sum -c -
	rm -rf $(LLVM_SOURCE).new
	mkdir -p $(LLVM_SOURCE).new
	tar -xf $(LLVM_DISTFILE) --strip-components=1 -C $(LLVM_SOURCE).new
	patch -d $(LLVM_SOURCE).new -p1 < $(LLVM_PATCH)
	echo '$(LLVM_SOURCE_ID)' > $(LLVM_SOURCE).new/.patched
	rm -rf $(LLVM_SOURCE)
	mv $(LLVM_SOURCE).new $(LLVM_SOURCE)

$(LLVM_INSTALL)/bin/clang:
	@if [ ! -x "$@" ]; then \
		$(MAKE) llvm-source; \
		mkdir -p $(LLVM_BUILD); \
		cd $(LLVM_BUILD) && cmake -G "Unix Makefiles" \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_INSTALL_PREFIX=$(LLVM_INSTALL) \
			-DLLVM_ENABLE_PROJECTS="clang;lld" \
			-DLLVM_TARGETS_TO_BUILD="X86;AArch64;ARM;RISCV" \
			-DLLVM_DISTRIBUTION_COMPONENTS="$(LLVM_DISTRIBUTION_COMPONENTS)" \
			$(LLVM_SOURCE)/llvm && \
		cmake --build $(LLVM_BUILD) --target install-distribution --parallel $(BUILD_JOBS); \
	fi

$(filter-out $(LLVM_INSTALL)/bin/clang,$(LLVM_INSTALLED_TOOLS)): $(LLVM_INSTALL)/bin/clang
