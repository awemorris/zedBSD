# zedBSD userland package: Chromium (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# Chromium is BSD-3-Clause (with third-party components under their own
# permissive/copyleft licences) and stays outside the source tree: only this
# Makefile, the recorded archive identity, GN/Rust target definitions and
# patches live here.

CHROMIUM_MAKEFILE := $(lastword $(MAKEFILE_LIST))
CHROMIUM_DIR := $(patsubst %/,%,$(dir $(CHROMIUM_MAKEFILE)))
include $(CHROMIUM_DIR)/../../base/package.mk
include $(CHROMIUM_DIR)/../external.mk

EXT_chromium_VERSION := 158.0.8127.65
EXT_chromium_ROOT := chromium-158.0.8127.65
EXT_chromium_ARCHIVE := chromium-158.0.8127.65.tar.xz
EXT_chromium_URL := https://commondatastorage.googleapis.com/chromium-browser-official/chromium-158.0.8127.65.tar.xz
EXT_chromium_SIZE := 4245264080
EXT_chromium_SHA256 := 0a208cde0fa39eb26fbbe62fa0c77bc2421869a2db374c196add5683c4b6129e
EXT_chromium_PATCH_LEVEL := zedbsd1
EXT_chromium_PATCHES := $(sort $(wildcard $(abspath $(CHROMIUM_DIR))/patches/*.patch))

$(eval $(call EXTERNAL_SOURCE,chromium))

# The GN build directory: args.gn names this tree's compiler, sysroot and
# shared libraries, and is written again whenever the template or a patch
# changes.  The GN and Ninja are the ones the release archive carries and the
# build machine's.
CHROMIUM_GN := $(EXT_chromium_SRCDIR)/buildtools/linux64/gn
CHROMIUM_LLVM := $(patsubst %/,%,$(dir $(EXTERNAL_LLVM_BIN)))
CHROMIUM_ARGS := $(EXT_chromium_BUILDDIR)/args.gn
CHROMIUM_TARGETS ?= content/shell:content_shell

$(CHROMIUM_ARGS): $(CHROMIUM_DIR)/args.gn.in $(EXT_chromium_SRCSTAMP)
	@mkdir -p '$(EXT_chromium_BUILDDIR)'
	sed -e 's|@LLVM@|$(CHROMIUM_LLVM)|' \
		-e 's|@SYSROOT@|$(EXTERNAL_SYSROOT)|' \
		-e 's|@DYNAMIC@|$(EXTERNAL_DYNAMIC)|' \
		'$(CHROMIUM_DIR)/args.gn.in' > '$@.tmp'
	@mv '$@.tmp' '$@'

.PHONY: chromium-gen chromium-build
chromium-gen: $(CHROMIUM_ARGS)
	cd '$(EXT_chromium_SRCDIR)' && '$(CHROMIUM_GN)' gen '$(EXT_chromium_BUILDDIR)' \
		--root-target=//content/shell:content_shell

chromium-build: chromium-gen
	$(EXTERNAL_NINJA_PROGRAM) -C '$(EXT_chromium_BUILDDIR)' -j $(BUILD_JOBS) \
		$(CHROMIUM_TARGETS)
