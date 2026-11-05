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

ifndef EXTERNAL_MK

EXTERNAL_MK := $(lastword $(MAKEFILE_LIST))

# The repository Makefile includes every package Makefile from the repository
# root; a standalone `make -C userland/packages/...` has package.mk set
# ZEDBSD_REPO_ROOT instead.
EXTERNAL_ROOT := $(if $(TOPLEVEL_BUILD),$(CURDIR),$(REPO_ROOT))
EXTERNAL_TOOLS := $(abspath $(EXTERNAL_ROOT)/userland/packages/tools)

# How many jobs a nested build tool (cmake, ninja) may run; the top-level
# build reads the same file, and a standalone package build gets it here.
include $(EXTERNAL_ROOT)/build-jobs.mk
EXTERNAL_ARCHIVE_SH := $(EXTERNAL_TOOLS)/archive.sh

# Archives are architecture-independent, so every configured build shares one
# distfiles directory.  Extracted trees and build directories are per package.
EXTERNAL_DISTDIR := $(abspath $(EXTERNAL_ROOT)/build/distfiles)

# The work trees hold what was built for one architecture, so each
# architecture has its own.  amd64 keeps the name it had before the others
# could build packages, so its trees are not built again.
EXTERNAL_WORKROOT := $(abspath $(EXTERNAL_ROOT)/build/packages$(if \
	$(filter-out amd64,$(ARCHITECTURE)),-$(ARCHITECTURE),))

# ---------------------------------------------------------------- cross build

# The configured target, derived here rather than taken from the repository
# Makefile, so that `make -C userland/packages/...` and the top-level build
# agree without depending on include order.
EXTERNAL_TRIPLE := $(strip \
	$(if $(filter amd64,$(ARCHITECTURE)),x86_64-unknown-zedbsd,\
	$(if $(filter i386,$(ARCHITECTURE)),i386-unknown-zedbsd,\
	$(if $(filter arm64,$(ARCHITECTURE)),aarch64-unknown-zedbsd,\
	$(if $(filter riscv64,$(ARCHITECTURE)),riscv64-unknown-zedbsd,\
	$(if $(filter armv7,$(ARCHITECTURE)),armv7-unknown-zedbsd-eabihf,))))))

# The LLVM backend of the configured target, for the packages built from LLVM.
EXTERNAL_LLVM_TARGET := $(strip \
	$(if $(filter arm64,$(ARCHITECTURE)),AArch64,\
	$(if $(filter riscv64,$(ARCHITECTURE)),RISCV,\
	$(if $(filter armv7,$(ARCHITECTURE)),ARM,X86))))
EXTERNAL_LLVM_BIN := $(abspath $(EXTERNAL_ROOT)/build/llvm/bin)
EXTERNAL_SYSROOT := \
	$(abspath $(EXTERNAL_ROOT)/build/$(ARCHITECTURE)/sysroot)
EXTERNAL_PLATFORM_DIR := $(strip \
	$(if $(TOPLEVEL_BUILD),$(PLATFORM_DIR),$(STANDALONE_PLATFORM_DIR)))
EXTERNAL_DYNAMIC := \
	$(abspath $(EXTERNAL_ROOT)/build/$(EXTERNAL_PLATFORM_DIR)/dynamic)

# One generated set of entry points per configured target: the wrappers, a
# CMake toolchain file with a platform description, an autoconf cross cache and
# an environment file.  A package uses these instead of naming compiler flags
# of its own, so the link contract lives in one place.
EXTERNAL_CROSS_DIR := \
	$(abspath $(EXTERNAL_ROOT)/build/$(EXTERNAL_PLATFORM_DIR)/packages/toolchain)
EXTERNAL_CROSS_ENV := $(EXTERNAL_CROSS_DIR)/environment.sh
EXTERNAL_CROSS_STAMP := $(EXTERNAL_CROSS_DIR)/.zedbsd-cross-toolchain
EXTERNAL_GEN_CROSS := $(EXTERNAL_TOOLS)/gen-cross-toolchain.sh
EXTERNAL_CROSS_INPUTS := $(EXTERNAL_GEN_CROSS) \
	$(EXTERNAL_SYSROOT)/.zedbsd-sysroot-complete

# The shared libraries the wrappers link against (BUG-081).  The packages are
# built once for every build directory, so the libraries they link against
# are not any one build directory's: they are copies kept beside the
# wrappers, taken from the build directory being built (from the default
# one when a package is built on its own).  Only the soname reaches a
# program, so any build of this tree's libraries serves.  The list is what
# the packages link: the C library, and libutil (openpty, for OpenSSH),
# available on the LP64 targets and armv7; a package that links another of this tree's
# shared libraries adds it here.  A copy is refreshed whenever its source
# changes, and the copies are order-only prerequisites of the wrappers: they
# have to exist before a package links, and a new C library does not
# configure the packages again.
EXTERNAL_LINK_DIR := $(EXTERNAL_CROSS_DIR)/lib
EXTERNAL_LINK_LIBS := libc.so $(if $(filter amd64 arm64 riscv64 armv7,$(ARCHITECTURE)),libutil.so)
EXTERNAL_LINK_SOURCE := $(if $(TOPLEVEL_BUILD),\
	$(BUILD)/dynamic,$(EXTERNAL_DYNAMIC))

$(EXTERNAL_LINK_DIR)/%.so: $(EXTERNAL_LINK_SOURCE)/%.so
	@mkdir -p '$(EXTERNAL_LINK_DIR)'
	cp '$<' '$@.tmp'
	@mv '$@.tmp' '$@'

$(EXTERNAL_CROSS_STAMP): $(EXTERNAL_CROSS_INPUTS) \
	| $(addprefix $(EXTERNAL_LINK_DIR)/,$(EXTERNAL_LINK_LIBS))
	$(EXTERNAL_GEN_CROSS) '$(EXTERNAL_TRIPLE)' \
		'$(EXTERNAL_LLVM_BIN)' '$(EXTERNAL_SYSROOT)' \
		'$(EXTERNAL_LINK_DIR)' '$(EXTERNAL_CROSS_DIR)'
	@touch '$@'

.PHONY: packages-cross-toolchain
packages-cross-toolchain: $(EXTERNAL_CROSS_STAMP)
	@:

# ---------------------------------------------------------------- Meson

# Meson is given the same wrappers through a cross file, a native file for
# the programs that run during the build, and a pkg-config that sees only the
# packages a build declares (gen-meson-cross.sh says why each is the way it
# is).  They are written beside the other entry points.
EXTERNAL_GEN_MESON := $(EXTERNAL_TOOLS)/gen-meson-cross.sh
EXTERNAL_MESON_CROSS := $(EXTERNAL_CROSS_DIR)/meson-cross.ini
EXTERNAL_MESON_NATIVE := $(EXTERNAL_CROSS_DIR)/meson-native.ini
EXTERNAL_PKG_CONFIG := $(EXTERNAL_CROSS_DIR)/bin/zedbsd-pkg-config
EXTERNAL_MESON_STAMP := $(EXTERNAL_CROSS_DIR)/.zedbsd-meson-cross

# The Meson and Ninja that run the builds.  The build machine's own are the
# default; a package set that needs a newer Meson names one here (for
# example `python3 <meson-src>/meson.py`) without changing any package.
EXTERNAL_MESON_PROGRAM ?= meson
EXTERNAL_NINJA_PROGRAM ?= ninja

# Directories of build-machine programs built from source (gperf, for
# example) that Meson builds find on PATH.  A host tool's Makefile adds its
# directory here and its program to EXT_<name>_HOST_TOOLS of the
# packages that run it.
EXTERNAL_HOST_BIN_DIRS :=

# A single space, for joining a list with no separator.
EXTERNAL_EMPTY :=
EXTERNAL_SPACE := $(EXTERNAL_EMPTY) $(EXTERNAL_EMPTY)

$(EXTERNAL_MESON_STAMP): $(EXTERNAL_CROSS_STAMP) $(EXTERNAL_GEN_MESON)
	$(EXTERNAL_GEN_MESON) '$(EXTERNAL_TRIPLE)' \
		'$(EXTERNAL_CROSS_DIR)'
	@touch '$@'

.PHONY: packages-meson-cross
packages-meson-cross: $(EXTERNAL_MESON_STAMP)
	@:

# The packages a package builds against, with the packages they build
# against in turn, each once.  $(1) = package names.  Every external package
# names its own in EXT_<name>_DEPENDS (none means none), so a package
# declares what it uses directly and the rest follows.
EXTERNAL_CLOSURE = $(if $(strip $(1)),$(sort $(1) $(call \
	EXTERNAL_CLOSURE,$(foreach dependency,$(1),$(EXT_$(dependency)_DEPENDS)))))

# Builds the view a package sees its dependencies through: one directory
# whose usr/ holds a symbolic link to every file the dependencies staged.
# $(1) = the view, $(2) = package names.  Two packages that stage the same
# file make cp fail, which is the conflict it is.
define EXTERNAL_VIEW
	@rm -rf '$(1)'
	@mkdir -p '$(1)/usr'
	@set -eu; for dependency in $(2); do \
		cp -as '$(EXTERNAL_WORKROOT)/'"$$dependency"'/stage/usr/.' '$(1)/usr/'; \
	done
endef

# The development files a package puts in the root beside its libraries, so
# that a program can be compiled against them on the running system: the
# headers, the .pc files and the unversioned name of each library the linker
# looks for.  $(1) = the program (installed only when it is selected),
# $(2) = its stage, $(3) = files under the stage's usr/ (a header, a .pc file),
# $(4) = directories under usr/ that go as a whole (a library's own include
# directory), $(5) = the libraries' SONAMEs, each of which gets its name
# without the version as a link to it in /usr/lib (libcairo.so.2 gives
# libcairo.so).  The stage is among the package's inputs through its own
# stamp, and the root is staged after it, so the files exist by then.
define EXTERNAL_DEVELOPMENT
PACKAGE_FILES += $$(if $$(filter $(1),$$(USER_PROGRAMS)),\
	$(foreach file,$(3),--file /usr/$(file)=$(2)/usr/$(file)) \
	$(foreach directory,$(4),--subtree /usr/$(directory)=$(2)/usr/$(directory)))
PACKAGE_LINKS += $$(if $$(filter $(1),$$(USER_PROGRAMS)),\
	$(foreach soname,$(5),/usr/lib/$(basename $(soname))=$(soname)))
endef

# A Meson package after EXTERNAL_SOURCE.  $(1) = package name.  It reads
#
#   EXT_<name>_MAKEFILE       the package's Makefile (its options live there)
#   EXT_<name>_MESON_OPTIONS  -D options, in addition to the common ones
#   EXT_<name>_DEPENDS        names of the external packages it builds
#                                    against directly
#   EXT_<name>_HOST_TOOLS     build-machine programs it runs (files)
#
# and gives EXT_<name>_CONFIGURED and EXT_<name>_STAGED, the
# stamps of a configured build tree and of the staged install
# (<stage>/usr/...).  Every package's stage carries its stamp at
# <stage>/.zedbsd-staged, which is what a dependent waits for.  The package
# itself checks what it staged (check-dynamic-elf.py) and packages it.
define EXTERNAL_MESON

EXT_$(1)_CONFIGURED := $$(EXT_$(1)_BUILDDIR)/.zedbsd-configured
EXT_$(1)_STAGED := $$(EXT_$(1)_STAGEDIR)/.zedbsd-staged
EXT_$(1)_VIEW := $$(EXT_$(1)_WORKDIR)/view
# The prerequisites are the direct dependencies, whose own stamps wait for
# theirs; the view is made from the whole closure, which is complete only
# once every package Makefile has been read, so it is computed in the recipe.
EXT_$(1)_ALL_DEPENDS = $$(call EXTERNAL_CLOSURE,$$(EXT_$(1)_DEPENDS))
EXT_$(1)_DEPENDS_STAGED := $$(foreach dependency,$$(EXT_$(1)_DEPENDS),\
	$$(EXTERNAL_WORKROOT)/$$(dependency)/stage/.zedbsd-staged)

# What every Meson command of this package runs with: its view for
# pkg-config, and the host tools ahead of the build machine's own programs.
EXT_$(1)_MESON_ENV = env \
	PKG_CONFIG_VIEW='$$(EXT_$(1)_VIEW)' \
	PATH="$$(subst $$(EXTERNAL_SPACE),,$$(foreach directory,$$(EXTERNAL_HOST_BIN_DIRS),$$(directory):))$$$$PATH"

# A changed option, dependency or cross file configures the tree again from
# nothing, so no option survives from an earlier configuration.  The view's
# lib/ is also given to the linker as the place to resolve the libraries the
# direct dependencies themselves need (-rpath-link records nothing in the
# output).
$$(EXT_$(1)_CONFIGURED): $$(EXT_$(1)_SRCSTAMP) \
	$$(EXTERNAL_MESON_STAMP) $$(EXT_$(1)_MAKEFILE) \
	$$(EXT_$(1)_DEPENDS_STAGED) $$(EXT_$(1)_HOST_TOOLS)
	@rm -rf '$$(EXT_$(1)_BUILDDIR)'
	$$(call EXTERNAL_VIEW,$$(EXT_$(1)_VIEW),$$(EXT_$(1)_ALL_DEPENDS))
	$$(EXT_$(1)_MESON_ENV) $$(EXTERNAL_MESON_PROGRAM) setup \
		'$$(EXT_$(1)_BUILDDIR)' '$$(EXT_$(1)_SRCDIR)' \
		--cross-file '$$(EXTERNAL_MESON_CROSS)' \
		--native-file '$$(EXTERNAL_MESON_NATIVE)' \
		--prefix=/usr --libdir=lib --buildtype=release \
		--wrap-mode=nodownload -Ddefault_library=shared \
		'-Dc_link_args=-Wl,-rpath-link,$$(EXT_$(1)_VIEW)/usr/lib' \
		'-Dcpp_link_args=-Wl,-rpath-link,$$(EXT_$(1)_VIEW)/usr/lib' \
		$$(EXT_$(1)_MESON_OPTIONS)
	@touch '$$@'

# The stage is replaced as a whole, so nothing from an earlier install stays.
$$(EXT_$(1)_STAGED): $$(EXT_$(1)_CONFIGURED)
	@rm -rf '$$(EXT_$(1)_STAGEDIR)'
	$$(EXT_$(1)_MESON_ENV) $$(EXTERNAL_NINJA_PROGRAM) \
		-C '$$(EXT_$(1)_BUILDDIR)' -j $$(BUILD_JOBS)
	$$(EXT_$(1)_MESON_ENV) $$(EXTERNAL_MESON_PROGRAM) install \
		-C '$$(EXT_$(1)_BUILDDIR)' --no-rebuild --quiet \
		--destdir '$$(EXT_$(1)_STAGEDIR)'
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
EXTERNAL_LLVM_SOURCE := $(abspath $(EXTERNAL_ROOT)/build/llvm-source)
EXTERNAL_LLVM_VERIFIED = \
	$(EXTERNAL_LLVM_SOURCE)/.zedbsd-source-verified-$(LLVM_VERSION)-$(LLVM_PATCH_LEVEL)

# The toolchain's directories are kept read-only (plan/tools/toolchain-lock.sh)
# and cp -a copies their modes, so the copy's directories are made writable
# before patch creates files in them, and before an earlier copy is removed.
# Only directories: a file is a link to the toolchain's own and shares its
# mode (BUG-126).
#
# $(1) = the copy, $(2) = the patches, applied in order.  A changed patch set
# or LLVM release makes a new copy from the verified tree.
define EXTERNAL_LLVM_COPY
	@set -eu; \
	copy='$(1)'; \
	for old in "$$copy" "$$copy.tmp"; do \
		if [ -d "$$old" ] && [ ! -L "$$old" ]; then \
			find "$$old" -type d -exec chmod u+w {} +; \
		fi; \
	done; \
	rm -rf "$$copy" "$$copy.tmp"; \
	mkdir -p "$${copy%/*}"; \
	cp -al '$(EXTERNAL_LLVM_SOURCE)/.' "$$copy.tmp"; \
	find "$$copy.tmp" -type d -exec chmod u+w {} +; \
	for patch in $(2); do \
		patch -p1 --batch --forward --fuzz=0 -d "$$copy.tmp" < "$$patch"; \
	done; \
	mv "$$copy.tmp" "$$copy"
endef

# $(1) = package name, as declared by EXT_<name>_* variables.
define EXTERNAL_SOURCE

EXT_$(1)_DISTFILE := $$(EXTERNAL_DISTDIR)/$$(EXT_$(1)_ARCHIVE)
EXT_$(1)_WORKDIR := $$(EXTERNAL_WORKROOT)/$(1)
EXT_$(1)_SRCDIR := $$(EXTERNAL_WORKROOT)/$(1)/src
EXT_$(1)_BUILDDIR := $$(EXTERNAL_WORKROOT)/$(1)/build
EXT_$(1)_STAGEDIR := $$(EXTERNAL_WORKROOT)/$(1)/stage

# Each record sits beside the thing it describes and names the version and
# patch level it was made for, so a changed declaration invalidates it and an
# unchanged one is not examined again.
EXT_$(1)_ARCHIVE_VERIFIED := \
	$$(EXTERNAL_DISTDIR)/.verified-$$(EXT_$(1)_ARCHIVE)
EXT_$(1)_SRCSTAMP := \
	$$(EXT_$(1)_SRCDIR)/.zedbsd-source-$$(EXT_$(1)_VERSION)-$$(EXT_$(1)_PATCH_LEVEL)

# Acquisition is the only step that touches the network, and it runs only when
# something explicitly asks for the archive.  Opening menuconfig or building an
# image that does not select this package performs no network I/O.
$$(EXT_$(1)_DISTFILE):
	$$(EXTERNAL_ARCHIVE_SH) fetch \
		'$$(EXT_$(1)_URL)' '$$@' \
		'$$(EXT_$(1)_SIZE)' '$$(EXT_$(1)_SHA256)' \
		'$$(EXT_$(1)_ROOT)'

# The record is older than the archive whenever the file is replaced, so a new
# archive is checked and an unchanged one is not read again.
$$(EXT_$(1)_ARCHIVE_VERIFIED): $$(EXT_$(1)_DISTFILE)
	$$(EXTERNAL_ARCHIVE_SH) verify '$$<' \
		'$$(EXT_$(1)_SIZE)' '$$(EXT_$(1)_SHA256)' \
		'$$(EXT_$(1)_ROOT)'
	@touch '$$@'

# Extraction replaces the whole tree.  A changed patch set therefore starts
# from upstream bytes again instead of patching an already patched tree.
$$(EXT_$(1)_SRCSTAMP): $$(EXT_$(1)_ARCHIVE_VERIFIED) \
	$$(EXT_$(1)_PATCHES) $$(EXTERNAL_ARCHIVE_SH)
	$$(EXTERNAL_ARCHIVE_SH) extract \
		'$$(EXT_$(1)_DISTFILE)' '$$(EXT_$(1)_ROOT)' \
		'$$(EXT_$(1)_SRCDIR)' $$(EXT_$(1)_PATCHES)
	@touch '$$@'

.PHONY: $(1)-download $(1)-source
$(1)-download: $$(EXT_$(1)_ARCHIVE_VERIFIED)
	@:
$(1)-source: $$(EXT_$(1)_SRCSTAMP)
	@:

USERLAND_DOWNLOAD_TARGETS += $(1)-download
USERLAND_PATCH_TARGETS += $(1)-source

endef

# $(1) = package name, for a release that is one plain file, not an archive.
# EXT_<name>_ARCHIVE names the file; it is fetched and checked by size
# and SHA-256 like an archive, and then used as it is.  There is no ROOT, no
# extraction and no patch.
define EXTERNAL_FILE

EXT_$(1)_DISTFILE := $$(EXTERNAL_DISTDIR)/$$(EXT_$(1)_ARCHIVE)
EXT_$(1)_WORKDIR := $$(EXTERNAL_WORKROOT)/$(1)
EXT_$(1)_ARCHIVE_VERIFIED := \
	$$(EXTERNAL_DISTDIR)/.verified-$$(EXT_$(1)_ARCHIVE)

$$(EXT_$(1)_DISTFILE):
	$$(EXTERNAL_ARCHIVE_SH) fetch \
		'$$(EXT_$(1)_URL)' '$$@' \
		'$$(EXT_$(1)_SIZE)' '$$(EXT_$(1)_SHA256)' -

$$(EXT_$(1)_ARCHIVE_VERIFIED): $$(EXT_$(1)_DISTFILE)
	$$(EXTERNAL_ARCHIVE_SH) verify '$$<' \
		'$$(EXT_$(1)_SIZE)' '$$(EXT_$(1)_SHA256)' -
	@touch '$$@'

.PHONY: $(1)-download
$(1)-download: $$(EXT_$(1)_ARCHIVE_VERIFIED)
	@:

USERLAND_DOWNLOAD_TARGETS += $(1)-download

endef

endif
