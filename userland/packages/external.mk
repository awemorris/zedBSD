# Common verified source-distribution lifecycle for external userland packages.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# A package under userland/packages/ that is built from an upstream release
# tarball declares the archive's identity and calls ZEDBSD_EXTERNAL_SOURCE.
# It gets, for <name>:
#
#   <name>-download    acquire and verify the archive (network, on request)
#   <name>-source      extracted and patched tree, as a stamped target
#   ZEDBSD_EXT_<name>_SRCDIR     where that tree is
#   ZEDBSD_EXT_<name>_SRCSTAMP   what a build should depend on
#
# The declared size and SHA-256 are the package's statement about immutable
# upstream bytes.  Nothing is extracted, and no name becomes visible, until
# those hold; the checks themselves live in tools/archive.sh so that they can
# be tested without a build.

ifndef ZEDBSD_EXTERNAL_MK
ZEDBSD_EXTERNAL_MK := $(lastword $(MAKEFILE_LIST))

# The repository Makefile includes every package Makefile from the repository
# root; a standalone `make -C userland/packages/...` has package.mk set
# ZEDBSD_REPO_ROOT instead.
ZEDBSD_EXTERNAL_ROOT := $(if $(ZEDBSD_TOPLEVEL_BUILD),$(CURDIR),$(ZEDBSD_REPO_ROOT))
ZEDBSD_EXTERNAL_TOOLS := $(abspath $(ZEDBSD_EXTERNAL_ROOT)/userland/packages/tools)
ZEDBSD_EXTERNAL_ARCHIVE_SH := $(ZEDBSD_EXTERNAL_TOOLS)/archive.sh

# Archives are architecture-independent, so every configured build shares one
# distfiles directory.  Extracted trees and build directories are per package.
ZEDBSD_EXTERNAL_DISTDIR := $(abspath $(ZEDBSD_EXTERNAL_ROOT)/build/distfiles)
ZEDBSD_EXTERNAL_WORKROOT := $(abspath $(ZEDBSD_EXTERNAL_ROOT)/build/packages)

# ---------------------------------------------------------------- cross build

# The configured target, derived here rather than taken from the repository
# Makefile, so that `make -C userland/packages/...` and the top-level build
# agree without depending on include order.
ZEDBSD_EXTERNAL_TRIPLE := $(strip \
	$(if $(filter amd64,$(ZEDBSD_ARCHITECTURE)),x86_64-unknown-zedbsd,\
	$(if $(filter i386,$(ZEDBSD_ARCHITECTURE)),i386-unknown-zedbsd,)))
ZEDBSD_EXTERNAL_LLVM_BIN := $(abspath $(ZEDBSD_EXTERNAL_ROOT)/build/llvm/bin)
ZEDBSD_EXTERNAL_SYSROOT := \
	$(abspath $(ZEDBSD_EXTERNAL_ROOT)/build/$(ZEDBSD_ARCHITECTURE)/sysroot)
ZEDBSD_EXTERNAL_PLATFORM_DIR := $(strip \
	$(if $(ZEDBSD_TOPLEVEL_BUILD),$(ZEDBSD_PLATFORM_DIR),$(ZEDBSD_STANDALONE_PLATFORM_DIR)))
ZEDBSD_EXTERNAL_DYNAMIC := \
	$(abspath $(ZEDBSD_EXTERNAL_ROOT)/build/$(ZEDBSD_EXTERNAL_PLATFORM_DIR)/dynamic)

# One generated set of entry points per configured target: the wrappers, a
# CMake toolchain file with a platform description, an autoconf cross cache and
# an environment file.  A package uses these instead of naming compiler flags
# of its own, so the link contract lives in one place.
ZEDBSD_EXTERNAL_CROSS_DIR := \
	$(abspath $(ZEDBSD_EXTERNAL_ROOT)/build/$(ZEDBSD_EXTERNAL_PLATFORM_DIR)/packages/toolchain)
ZEDBSD_EXTERNAL_CROSS_ENV := $(ZEDBSD_EXTERNAL_CROSS_DIR)/environment.sh
ZEDBSD_EXTERNAL_CROSS_STAMP := $(ZEDBSD_EXTERNAL_CROSS_DIR)/.zedbsd-cross-toolchain
ZEDBSD_EXTERNAL_GEN_CROSS := $(ZEDBSD_EXTERNAL_TOOLS)/gen-cross-toolchain.sh
ZEDBSD_EXTERNAL_CROSS_INPUTS := $(ZEDBSD_EXTERNAL_GEN_CROSS) \
	$(ZEDBSD_EXTERNAL_SYSROOT)/.zedbsd-sysroot-complete

$(ZEDBSD_EXTERNAL_CROSS_STAMP): $(ZEDBSD_EXTERNAL_CROSS_INPUTS)
	$(ZEDBSD_EXTERNAL_GEN_CROSS) '$(ZEDBSD_EXTERNAL_TRIPLE)' \
		'$(ZEDBSD_EXTERNAL_LLVM_BIN)' '$(ZEDBSD_EXTERNAL_SYSROOT)' \
		'$(ZEDBSD_EXTERNAL_DYNAMIC)' '$(ZEDBSD_EXTERNAL_CROSS_DIR)'
	@touch '$@'

.PHONY: packages-cross-toolchain
packages-cross-toolchain: $(ZEDBSD_EXTERNAL_CROSS_STAMP)
	@:

# $(1) = package name, as declared by ZEDBSD_EXT_<name>_* variables.
define ZEDBSD_EXTERNAL_SOURCE

ZEDBSD_EXT_$(1)_DISTFILE := $$(ZEDBSD_EXTERNAL_DISTDIR)/$$(ZEDBSD_EXT_$(1)_ARCHIVE)
ZEDBSD_EXT_$(1)_WORKDIR := $$(ZEDBSD_EXTERNAL_WORKROOT)/$(1)
ZEDBSD_EXT_$(1)_SRCDIR := $$(ZEDBSD_EXTERNAL_WORKROOT)/$(1)/src
ZEDBSD_EXT_$(1)_BUILDDIR := $$(ZEDBSD_EXTERNAL_WORKROOT)/$(1)/build
ZEDBSD_EXT_$(1)_STAGEDIR := $$(ZEDBSD_EXTERNAL_WORKROOT)/$(1)/stage

# Each record sits beside the thing it describes and names the version and
# patch level it was made for, so a changed declaration invalidates it and an
# unchanged one is not examined again.
ZEDBSD_EXT_$(1)_ARCHIVE_VERIFIED := \
	$$(ZEDBSD_EXTERNAL_DISTDIR)/.verified-$$(ZEDBSD_EXT_$(1)_ARCHIVE)
ZEDBSD_EXT_$(1)_SRCSTAMP := \
	$$(ZEDBSD_EXT_$(1)_SRCDIR)/.zedbsd-source-$$(ZEDBSD_EXT_$(1)_VERSION)-$$(ZEDBSD_EXT_$(1)_PATCH_LEVEL)

# Acquisition is the only step that touches the network, and it runs only when
# something explicitly asks for the archive.  Opening menuconfig or building an
# image that does not select this package performs no network I/O.
$$(ZEDBSD_EXT_$(1)_DISTFILE):
	$$(ZEDBSD_EXTERNAL_ARCHIVE_SH) fetch \
		'$$(ZEDBSD_EXT_$(1)_URL)' '$$@' \
		'$$(ZEDBSD_EXT_$(1)_SIZE)' '$$(ZEDBSD_EXT_$(1)_SHA256)' \
		'$$(ZEDBSD_EXT_$(1)_ROOT)'

# The record is older than the archive whenever the file is replaced, so a new
# archive is checked and an unchanged one is not read again.
$$(ZEDBSD_EXT_$(1)_ARCHIVE_VERIFIED): $$(ZEDBSD_EXT_$(1)_DISTFILE)
	$$(ZEDBSD_EXTERNAL_ARCHIVE_SH) verify '$$<' \
		'$$(ZEDBSD_EXT_$(1)_SIZE)' '$$(ZEDBSD_EXT_$(1)_SHA256)' \
		'$$(ZEDBSD_EXT_$(1)_ROOT)'
	@touch '$$@'

# Extraction replaces the whole tree.  A changed patch set therefore starts
# from upstream bytes again instead of patching an already patched tree.
$$(ZEDBSD_EXT_$(1)_SRCSTAMP): $$(ZEDBSD_EXT_$(1)_ARCHIVE_VERIFIED) \
	$$(ZEDBSD_EXT_$(1)_PATCHES) $$(ZEDBSD_EXTERNAL_ARCHIVE_SH)
	$$(ZEDBSD_EXTERNAL_ARCHIVE_SH) extract \
		'$$(ZEDBSD_EXT_$(1)_DISTFILE)' '$$(ZEDBSD_EXT_$(1)_ROOT)' \
		'$$(ZEDBSD_EXT_$(1)_SRCDIR)' $$(ZEDBSD_EXT_$(1)_PATCHES)
	@touch '$$@'

.PHONY: $(1)-download $(1)-source
$(1)-download: $$(ZEDBSD_EXT_$(1)_ARCHIVE_VERIFIED)
	@:
$(1)-source: $$(ZEDBSD_EXT_$(1)_SRCSTAMP)
	@:

ZEDBSD_USERLAND_DOWNLOAD_TARGETS += $(1)-download
ZEDBSD_USERLAND_PATCH_TARGETS += $(1)-source

endef

# $(1) = package name, for a release that is one plain file, not an archive.
# ZEDBSD_EXT_<name>_ARCHIVE names the file; it is fetched and checked by size
# and SHA-256 like an archive, and then used as it is.  There is no ROOT, no
# extraction and no patch.
define ZEDBSD_EXTERNAL_FILE

ZEDBSD_EXT_$(1)_DISTFILE := $$(ZEDBSD_EXTERNAL_DISTDIR)/$$(ZEDBSD_EXT_$(1)_ARCHIVE)
ZEDBSD_EXT_$(1)_WORKDIR := $$(ZEDBSD_EXTERNAL_WORKROOT)/$(1)
ZEDBSD_EXT_$(1)_ARCHIVE_VERIFIED := \
	$$(ZEDBSD_EXTERNAL_DISTDIR)/.verified-$$(ZEDBSD_EXT_$(1)_ARCHIVE)

$$(ZEDBSD_EXT_$(1)_DISTFILE):
	$$(ZEDBSD_EXTERNAL_ARCHIVE_SH) fetch \
		'$$(ZEDBSD_EXT_$(1)_URL)' '$$@' \
		'$$(ZEDBSD_EXT_$(1)_SIZE)' '$$(ZEDBSD_EXT_$(1)_SHA256)' -

$$(ZEDBSD_EXT_$(1)_ARCHIVE_VERIFIED): $$(ZEDBSD_EXT_$(1)_DISTFILE)
	$$(ZEDBSD_EXTERNAL_ARCHIVE_SH) verify '$$<' \
		'$$(ZEDBSD_EXT_$(1)_SIZE)' '$$(ZEDBSD_EXT_$(1)_SHA256)' -
	@touch '$$@'

.PHONY: $(1)-download
$(1)-download: $$(ZEDBSD_EXT_$(1)_ARCHIVE_VERIFIED)
	@:

ZEDBSD_USERLAND_DOWNLOAD_TARGETS += $(1)-download

endef

endif
