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

# How many jobs a nested build tool (cmake, ninja) may run; the top-level
# build reads the same file, and a standalone package build gets it here.
include $(ZEDBSD_EXTERNAL_ROOT)/build-jobs.mk
ZEDBSD_EXTERNAL_ARCHIVE_SH := $(ZEDBSD_EXTERNAL_TOOLS)/archive.sh

# Archives are architecture-independent, so every configured build shares one
# distfiles directory.  Extracted trees and build directories are per package.
ZEDBSD_EXTERNAL_DISTDIR := $(abspath $(ZEDBSD_EXTERNAL_ROOT)/build/distfiles)
# The work trees hold what was built for one architecture, so each
# architecture has its own.  amd64 keeps the name it had before the others
# could build packages, so its trees are not built again.
ZEDBSD_EXTERNAL_WORKROOT := $(abspath $(ZEDBSD_EXTERNAL_ROOT)/build/packages$(if \
	$(filter-out amd64,$(ZEDBSD_ARCHITECTURE)),-$(ZEDBSD_ARCHITECTURE),))

# ---------------------------------------------------------------- cross build

# The configured target, derived here rather than taken from the repository
# Makefile, so that `make -C userland/packages/...` and the top-level build
# agree without depending on include order.
ZEDBSD_EXTERNAL_TRIPLE := $(strip \
	$(if $(filter amd64,$(ZEDBSD_ARCHITECTURE)),x86_64-unknown-zedbsd,\
	$(if $(filter i386,$(ZEDBSD_ARCHITECTURE)),i386-unknown-zedbsd,\
	$(if $(filter arm64,$(ZEDBSD_ARCHITECTURE)),aarch64-unknown-zedbsd,))))

# The LLVM backend of the configured target, for the packages built from LLVM.
ZEDBSD_EXTERNAL_LLVM_TARGET := $(strip \
	$(if $(filter arm64,$(ZEDBSD_ARCHITECTURE)),AArch64,X86))
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

# The shared libraries the wrappers link against (BUG-081).  The packages are
# built once for every build directory, so the libraries they link against
# are not any one build directory's: they are copies kept beside the
# wrappers, taken from the build directory being built (from the default
# one when a package is built on its own).  Only the soname reaches a
# program, so any build of this tree's libraries serves.  The list is what
# the packages link: the C library, and libutil (openpty, for OpenSSH),
# available on both LP64 targets; a package that links another of this tree's
# shared libraries adds it here.  A copy is refreshed whenever its source
# changes, and the copies are order-only prerequisites of the wrappers: they
# have to exist before a package links, and a new C library does not
# configure the packages again.
ZEDBSD_EXTERNAL_LINK_DIR := $(ZEDBSD_EXTERNAL_CROSS_DIR)/lib
ZEDBSD_EXTERNAL_LINK_LIBS := libc.so $(if $(filter amd64 arm64,$(ZEDBSD_ARCHITECTURE)),libutil.so)
ZEDBSD_EXTERNAL_LINK_SOURCE := $(if $(ZEDBSD_TOPLEVEL_BUILD),\
	$(BUILD)/dynamic,$(ZEDBSD_EXTERNAL_DYNAMIC))

$(ZEDBSD_EXTERNAL_LINK_DIR)/%.so: $(ZEDBSD_EXTERNAL_LINK_SOURCE)/%.so
	@mkdir -p '$(ZEDBSD_EXTERNAL_LINK_DIR)'
	cp '$<' '$@.tmp'
	@mv '$@.tmp' '$@'

$(ZEDBSD_EXTERNAL_CROSS_STAMP): $(ZEDBSD_EXTERNAL_CROSS_INPUTS) \
	| $(addprefix $(ZEDBSD_EXTERNAL_LINK_DIR)/,$(ZEDBSD_EXTERNAL_LINK_LIBS))
	$(ZEDBSD_EXTERNAL_GEN_CROSS) '$(ZEDBSD_EXTERNAL_TRIPLE)' \
		'$(ZEDBSD_EXTERNAL_LLVM_BIN)' '$(ZEDBSD_EXTERNAL_SYSROOT)' \
		'$(ZEDBSD_EXTERNAL_LINK_DIR)' '$(ZEDBSD_EXTERNAL_CROSS_DIR)'
	@touch '$@'

.PHONY: packages-cross-toolchain
packages-cross-toolchain: $(ZEDBSD_EXTERNAL_CROSS_STAMP)
	@:

# ---------------------------------------------------------------- Meson

# Meson is given the same wrappers through a cross file, a native file for
# the programs that run during the build, and a pkg-config that sees only the
# packages a build declares (gen-meson-cross.sh says why each is the way it
# is).  They are written beside the other entry points.
ZEDBSD_EXTERNAL_GEN_MESON := $(ZEDBSD_EXTERNAL_TOOLS)/gen-meson-cross.sh
ZEDBSD_EXTERNAL_MESON_CROSS := $(ZEDBSD_EXTERNAL_CROSS_DIR)/meson-cross.ini
ZEDBSD_EXTERNAL_MESON_NATIVE := $(ZEDBSD_EXTERNAL_CROSS_DIR)/meson-native.ini
ZEDBSD_EXTERNAL_PKG_CONFIG := $(ZEDBSD_EXTERNAL_CROSS_DIR)/bin/zedbsd-pkg-config
ZEDBSD_EXTERNAL_MESON_STAMP := $(ZEDBSD_EXTERNAL_CROSS_DIR)/.zedbsd-meson-cross

# The Meson and Ninja that run the builds.  The build machine's own are the
# default; a package set that needs a newer Meson names one here (for
# example `python3 <meson-src>/meson.py`) without changing any package.
ZEDBSD_EXTERNAL_MESON_PROGRAM ?= meson
ZEDBSD_EXTERNAL_NINJA_PROGRAM ?= ninja

# Directories of build-machine programs built from source (gperf, for
# example) that Meson builds find on PATH.  A host tool's Makefile adds its
# directory here and its program to ZEDBSD_EXT_<name>_HOST_TOOLS of the
# packages that run it.
ZEDBSD_EXTERNAL_HOST_BIN_DIRS :=

# A single space, for joining a list with no separator.
ZEDBSD_EXTERNAL_EMPTY :=
ZEDBSD_EXTERNAL_SPACE := $(ZEDBSD_EXTERNAL_EMPTY) $(ZEDBSD_EXTERNAL_EMPTY)

$(ZEDBSD_EXTERNAL_MESON_STAMP): $(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_EXTERNAL_GEN_MESON)
	$(ZEDBSD_EXTERNAL_GEN_MESON) '$(ZEDBSD_EXTERNAL_TRIPLE)' \
		'$(ZEDBSD_EXTERNAL_CROSS_DIR)'
	@touch '$@'

.PHONY: packages-meson-cross
packages-meson-cross: $(ZEDBSD_EXTERNAL_MESON_STAMP)
	@:

# The packages a package builds against, with the packages they build
# against in turn, each once.  $(1) = package names.  Every external package
# names its own in ZEDBSD_EXT_<name>_DEPENDS (none means none), so a package
# declares what it uses directly and the rest follows.
ZEDBSD_EXTERNAL_CLOSURE = $(if $(strip $(1)),$(sort $(1) $(call \
	ZEDBSD_EXTERNAL_CLOSURE,$(foreach dependency,$(1),$(ZEDBSD_EXT_$(dependency)_DEPENDS)))))

# Builds the view a package sees its dependencies through: one directory
# whose usr/ holds a symbolic link to every file the dependencies staged.
# $(1) = the view, $(2) = package names.  Two packages that stage the same
# file make cp fail, which is the conflict it is.
define ZEDBSD_EXTERNAL_VIEW
	@rm -rf '$(1)'
	@mkdir -p '$(1)/usr'
	@set -eu; for dependency in $(2); do \
		cp -as '$(ZEDBSD_EXTERNAL_WORKROOT)/'"$$dependency"'/stage/usr/.' '$(1)/usr/'; \
	done
endef

# A Meson package after ZEDBSD_EXTERNAL_SOURCE.  $(1) = package name.  It reads
#
#   ZEDBSD_EXT_<name>_MAKEFILE       the package's Makefile (its options live there)
#   ZEDBSD_EXT_<name>_MESON_OPTIONS  -D options, in addition to the common ones
#   ZEDBSD_EXT_<name>_DEPENDS        names of the external packages it builds
#                                    against directly
#   ZEDBSD_EXT_<name>_HOST_TOOLS     build-machine programs it runs (files)
#
# and gives ZEDBSD_EXT_<name>_CONFIGURED and ZEDBSD_EXT_<name>_STAGED, the
# stamps of a configured build tree and of the staged install
# (<stage>/usr/...).  Every package's stage carries its stamp at
# <stage>/.zedbsd-staged, which is what a dependent waits for.  The package
# itself checks what it staged (check-dynamic-elf.py) and packages it.
define ZEDBSD_EXTERNAL_MESON

ZEDBSD_EXT_$(1)_CONFIGURED := $$(ZEDBSD_EXT_$(1)_BUILDDIR)/.zedbsd-configured
ZEDBSD_EXT_$(1)_STAGED := $$(ZEDBSD_EXT_$(1)_STAGEDIR)/.zedbsd-staged
ZEDBSD_EXT_$(1)_VIEW := $$(ZEDBSD_EXT_$(1)_WORKDIR)/view
# The prerequisites are the direct dependencies, whose own stamps wait for
# theirs; the view is made from the whole closure, which is complete only
# once every package Makefile has been read, so it is computed in the recipe.
ZEDBSD_EXT_$(1)_ALL_DEPENDS = $$(call ZEDBSD_EXTERNAL_CLOSURE,$$(ZEDBSD_EXT_$(1)_DEPENDS))
ZEDBSD_EXT_$(1)_DEPENDS_STAGED := $$(foreach dependency,$$(ZEDBSD_EXT_$(1)_DEPENDS),\
	$$(ZEDBSD_EXTERNAL_WORKROOT)/$$(dependency)/stage/.zedbsd-staged)

# What every Meson command of this package runs with: its view for
# pkg-config, and the host tools ahead of the build machine's own programs.
ZEDBSD_EXT_$(1)_MESON_ENV = env \
	ZEDBSD_PKG_CONFIG_VIEW='$$(ZEDBSD_EXT_$(1)_VIEW)' \
	PATH="$$(subst $$(ZEDBSD_EXTERNAL_SPACE),,$$(foreach directory,$$(ZEDBSD_EXTERNAL_HOST_BIN_DIRS),$$(directory):))$$$$PATH"

# A changed option, dependency or cross file configures the tree again from
# nothing, so no option survives from an earlier configuration.  The view's
# lib/ is also given to the linker as the place to resolve the libraries the
# direct dependencies themselves need (-rpath-link records nothing in the
# output).
$$(ZEDBSD_EXT_$(1)_CONFIGURED): $$(ZEDBSD_EXT_$(1)_SRCSTAMP) \
	$$(ZEDBSD_EXTERNAL_MESON_STAMP) $$(ZEDBSD_EXT_$(1)_MAKEFILE) \
	$$(ZEDBSD_EXT_$(1)_DEPENDS_STAGED) $$(ZEDBSD_EXT_$(1)_HOST_TOOLS)
	@rm -rf '$$(ZEDBSD_EXT_$(1)_BUILDDIR)'
	$$(call ZEDBSD_EXTERNAL_VIEW,$$(ZEDBSD_EXT_$(1)_VIEW),$$(ZEDBSD_EXT_$(1)_ALL_DEPENDS))
	$$(ZEDBSD_EXT_$(1)_MESON_ENV) $$(ZEDBSD_EXTERNAL_MESON_PROGRAM) setup \
		'$$(ZEDBSD_EXT_$(1)_BUILDDIR)' '$$(ZEDBSD_EXT_$(1)_SRCDIR)' \
		--cross-file '$$(ZEDBSD_EXTERNAL_MESON_CROSS)' \
		--native-file '$$(ZEDBSD_EXTERNAL_MESON_NATIVE)' \
		--prefix=/usr --libdir=lib --buildtype=release \
		--wrap-mode=nodownload -Ddefault_library=shared \
		'-Dc_link_args=-Wl,-rpath-link,$$(ZEDBSD_EXT_$(1)_VIEW)/usr/lib' \
		'-Dcpp_link_args=-Wl,-rpath-link,$$(ZEDBSD_EXT_$(1)_VIEW)/usr/lib' \
		$$(ZEDBSD_EXT_$(1)_MESON_OPTIONS)
	@touch '$$@'

# The stage is replaced as a whole, so nothing from an earlier install stays.
$$(ZEDBSD_EXT_$(1)_STAGED): $$(ZEDBSD_EXT_$(1)_CONFIGURED)
	@rm -rf '$$(ZEDBSD_EXT_$(1)_STAGEDIR)'
	$$(ZEDBSD_EXT_$(1)_MESON_ENV) $$(ZEDBSD_EXTERNAL_NINJA_PROGRAM) \
		-C '$$(ZEDBSD_EXT_$(1)_BUILDDIR)' -j $$(ZEDBSD_BUILD_JOBS)
	$$(ZEDBSD_EXT_$(1)_MESON_ENV) $$(ZEDBSD_EXTERNAL_MESON_PROGRAM) install \
		-C '$$(ZEDBSD_EXT_$(1)_BUILDDIR)' --no-rebuild --quiet \
		--destdir '$$(ZEDBSD_EXT_$(1)_STAGEDIR)'
	@touch '$$@'

endef

# ------------------------------------------------------- the LLVM source tree

# The packages built from LLVM (the C++ runtime, the compiler) use the source
# tree the toolchain build extracted and verified.  That tree is the
# toolchain's: it is checked against the release and its patch whenever it is
# extracted, so a package never patches it.  Each gets a copy of its own and
# patches that.  The copy is made of hard links, which costs no space; patch
# replaces a file it changes rather than writing into it, so the toolchain's
# names keep the verified contents.  The copy is taken of the tree's contents
# (`/.`): build/llvm-source may be a link to another checkout's tree, and a
# copy of the link would let patch write into that tree (BUG-089).
ZEDBSD_EXTERNAL_LLVM_SOURCE := $(abspath $(ZEDBSD_EXTERNAL_ROOT)/build/llvm-source)
ZEDBSD_EXTERNAL_LLVM_VERIFIED = \
	$(ZEDBSD_EXTERNAL_LLVM_SOURCE)/.zedbsd-source-verified-$(ZEDBSD_LLVM_VERSION)-$(ZEDBSD_LLVM_PATCH_LEVEL)

# The toolchain's directories are kept read-only (plan/tools/toolchain-lock.sh)
# and cp -a copies their modes, so the copy's directories are made writable
# before patch creates files in them, and before an earlier copy is removed.
# Only directories: a file is a link to the toolchain's own and shares its
# mode (BUG-126).
#
# $(1) = the copy, $(2) = the patches, applied in order.  A changed patch set
# or LLVM release makes a new copy from the verified tree.
define ZEDBSD_EXTERNAL_LLVM_COPY
	@set -eu; \
	copy='$(1)'; \
	for old in "$$copy" "$$copy.tmp"; do \
		if [ -d "$$old" ] && [ ! -L "$$old" ]; then \
			find "$$old" -type d -exec chmod u+w {} +; \
		fi; \
	done; \
	rm -rf "$$copy" "$$copy.tmp"; \
	mkdir -p "$${copy%/*}"; \
	cp -al '$(ZEDBSD_EXTERNAL_LLVM_SOURCE)/.' "$$copy.tmp"; \
	find "$$copy.tmp" -type d -exec chmod u+w {} +; \
	for patch in $(2); do \
		patch -p1 --batch --forward --fuzz=0 -d "$$copy.tmp" < "$$patch"; \
	done; \
	mv "$$copy.tmp" "$$copy"
endef

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
