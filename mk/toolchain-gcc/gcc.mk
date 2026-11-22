# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# GCC toolchain configuration and build
#
# The architectures the project's LLVM does not serve are built with GCC
# and binutils, made here from the GNU release tarballs for a freestanding
# ELF target.  zedBSD brings its own libc, so no C library is built with
# them: the compiler is C only, with its runtime library (libgcc).  The
# tools are installed under build/gcc with the target's prefix
# (m68k-elf-gcc, m68k-elf-ld, ...).
#
# GCC is patched (patches/) so that the freestanding target's C types are
# those of zedBSD's other 32-bit ports: size_t, ptrdiff_t, intptr_t and
# the 32-bit fixed-width types are int types, not long ones.
#

GCC_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
include $(GCC_DIR)/version.mk

REPO_ROOT ?= $(abspath $(GCC_DIR)/../..)
include $(REPO_ROOT)/mk/build-jobs.mk

# The target.  m68k is the only architecture built with GCC; its multilibs
# are the 680x0 processors (no ColdFire), each also as soft-float.
GCC_TARGET := m68k-elf
GCC_TARGET_OPTIONS := --with-arch=m68k

GCC_DISTDIR := $(GCC_DIR)/distfiles
GCC_SOURCE := $(REPO_ROOT)/build/gcc-source
BINUTILS_SOURCE := $(REPO_ROOT)/build/binutils-source
GCC_BUILD := $(REPO_ROOT)/build/gcc-build/$(GCC_TARGET)
GCC_INSTALL := $(REPO_ROOT)/build/gcc
GCC_BIN := $(GCC_INSTALL)/bin

# Toolchain commands for the platforms whose defaults.mk chooses GCC
# (PLATFORM_TOOLCHAIN := gcc); the others keep the LLVM commands.
ifeq ($(PLATFORM_TOOLCHAIN),gcc)
CC      := $(GCC_BIN)/$(GCC_TARGET)-gcc
CXX     :=
AS      := $(CC)
LD      := $(GCC_BIN)/$(GCC_TARGET)-ld
AR      := $(GCC_BIN)/$(GCC_TARGET)-ar
RANLIB  := $(GCC_BIN)/$(GCC_TARGET)-ranlib
NM      := $(GCC_BIN)/$(GCC_TARGET)-nm
OBJCOPY := $(GCC_BIN)/$(GCC_TARGET)-objcopy
OBJDUMP := $(GCC_BIN)/$(GCC_TARGET)-objdump
READELF := $(GCC_BIN)/$(GCC_TARGET)-readelf
STRIP   := $(GCC_BIN)/$(GCC_TARGET)-strip
CFLAGS  :=
endif

BINUTILS_DISTFILE := $(GCC_DISTDIR)/$(ZEDBSD_BINUTILS_ARCHIVE_NAME)
GCC_DISTFILE := $(GCC_DISTDIR)/$(ZEDBSD_GCC_ARCHIVE_NAME)
GMP_DISTFILE := $(GCC_DISTDIR)/$(ZEDBSD_GMP_ARCHIVE_NAME)
MPFR_DISTFILE := $(GCC_DISTDIR)/$(ZEDBSD_MPFR_ARCHIVE_NAME)
MPC_DISTFILE := $(GCC_DISTDIR)/$(ZEDBSD_MPC_ARCHIVE_NAME)
GCC_PATCHES := $(sort $(wildcard $(GCC_DIR)/patches/*.patch))

# The host build: optimized without debug information, documentation not
# made (makeinfo is not needed).
GCC_HOST_FLAGS := CFLAGS=-O2 CXXFLAGS=-O2 MAKEINFO=true

# The configure options.  The prefix is given apart from them, so that
# what the toolchain is built from does not name the directory it is
# installed in: the installed tools find each other relative to their own
# place, and a prebuilt toolchain extracted in another tree matches.
BINUTILS_CONFIGURE_OPTIONS := \
	--target=$(GCC_TARGET) \
	--disable-nls \
	--disable-werror \
	--disable-gdb \
	--disable-gdbserver \
	--disable-sim \
	--disable-gprofng \
	--disable-readline \
	--disable-libdecnumber \
	--without-zstd \
	--without-debuginfod \
	--enable-deterministic-archives

GCC_CONFIGURE_OPTIONS := \
	--target=$(GCC_TARGET) \
	$(GCC_TARGET_OPTIONS) \
	--enable-languages=c \
	--without-headers \
	--disable-shared \
	--disable-threads \
	--disable-nls \
	--disable-lto \
	--disable-libssp \
	--disable-libquadmath \
	--disable-libgomp \
	--disable-libatomic \
	--disable-libitm \
	--disable-libvtv \
	--disable-libsanitizer \
	--without-isl \
	--without-zstd

# The nested builds run on their own: the caller's flags and variables
# (-r, PLATFORM, BUILD) are not theirs, and they get the job count of
# mk/build-jobs.mk.  The make command is copied when this file is read, so
# that "make -n" prints these lines and does not run them (as it would run
# a line that names $(MAKE) itself, without its -n).
GCC_MAKE := $(MAKE)
GCC_SUBMAKE = env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL -u MAKEOVERRIDES \
	PATH='$(GCC_BIN)':"$$PATH" $(GCC_MAKE) -j$(BUILD_JOBS)

.PHONY: toolchain-gcc gcc-download gcc-source binutils-source

toolchain-gcc: $(GCC_INSTALL)/.zedbsd-$(GCC_TARGET)
	@'$(GCC_BIN)/$(GCC_TARGET)-gcc' -dumpversion | grep -Fx '$(ZEDBSD_GCC_VERSION)' >/dev/null
	@'$(GCC_BIN)/$(GCC_TARGET)-ld' --version | grep -F '$(ZEDBSD_BINUTILS_VERSION)' >/dev/null

# Fetch a tarball when it is not here yet, and check its SHA-256.
# $(1): the distfile, $(2): its URL, $(3): its SHA-256.
define gcc-fetch
	@if [ ! -f $(1) ]; then \
		mkdir -p $(dir $(1)) && \
		curl --fail --location --output $(1).part '$(2)' && \
		mv $(1).part $(1); \
	fi
	@echo '$(3)  $(1)' | sha256sum -c -
endef

gcc-download:
	$(call gcc-fetch,$(BINUTILS_DISTFILE),$(ZEDBSD_BINUTILS_ARCHIVE_URL),$(ZEDBSD_BINUTILS_ARCHIVE_SHA256))
	$(call gcc-fetch,$(GCC_DISTFILE),$(ZEDBSD_GCC_ARCHIVE_URL),$(ZEDBSD_GCC_ARCHIVE_SHA256))
	$(call gcc-fetch,$(GMP_DISTFILE),$(ZEDBSD_GMP_ARCHIVE_URL),$(ZEDBSD_GMP_ARCHIVE_SHA256))
	$(call gcc-fetch,$(MPFR_DISTFILE),$(ZEDBSD_MPFR_ARCHIVE_URL),$(ZEDBSD_MPFR_ARCHIVE_SHA256))
	$(call gcc-fetch,$(MPC_DISTFILE),$(ZEDBSD_MPC_ARCHIVE_URL),$(ZEDBSD_MPC_ARCHIVE_SHA256))

binutils-source: $(BINUTILS_SOURCE)/.extracted
gcc-source: $(GCC_SOURCE)/.extracted

# The extracted sources are named by the archives' and the patches'
# hashes, not by file times, as the LLVM source is: a source that matches
# is used as it is, and the archives are fetched only when a source has to
# be made again.  A new source is extracted and patched beside the old one
# and then takes its place.
BINUTILS_SOURCE_ID := $(ZEDBSD_BINUTILS_ARCHIVE_SHA256)
GCC_SOURCE_ID := $(ZEDBSD_GCC_ARCHIVE_SHA256) $(ZEDBSD_GMP_ARCHIVE_SHA256) \
	$(ZEDBSD_MPFR_ARCHIVE_SHA256) $(ZEDBSD_MPC_ARCHIVE_SHA256) \
	$(foreach patch,$(GCC_PATCHES),$(firstword $(shell sha256sum $(patch))))

# The binutils source is made again when its archive's hash changed.
ifneq ($(shell cat $(BINUTILS_SOURCE)/.extracted 2>/dev/null),$(BINUTILS_SOURCE_ID))
$(BINUTILS_SOURCE)/.extracted: gcc-source-stale
endif

# The GCC source (with GMP, MPFR and MPC in its tree, which GCC builds
# with itself) is made again when one of the archives' hashes changed.
ifneq ($(shell cat $(GCC_SOURCE)/.extracted 2>/dev/null),$(GCC_SOURCE_ID))
$(GCC_SOURCE)/.extracted: gcc-source-stale
endif

.PHONY: gcc-source-stale
gcc-source-stale:

$(BINUTILS_SOURCE)/.extracted:
	$(call gcc-fetch,$(BINUTILS_DISTFILE),$(ZEDBSD_BINUTILS_ARCHIVE_URL),$(ZEDBSD_BINUTILS_ARCHIVE_SHA256))
	rm -rf $(BINUTILS_SOURCE).new
	mkdir -p $(BINUTILS_SOURCE).new
	tar -xf $(BINUTILS_DISTFILE) --strip-components=1 -C $(BINUTILS_SOURCE).new
	echo '$(BINUTILS_SOURCE_ID)' > $(BINUTILS_SOURCE).new/.extracted
	rm -rf $(BINUTILS_SOURCE)
	mv $(BINUTILS_SOURCE).new $(BINUTILS_SOURCE)

$(GCC_SOURCE)/.extracted:
	$(call gcc-fetch,$(GCC_DISTFILE),$(ZEDBSD_GCC_ARCHIVE_URL),$(ZEDBSD_GCC_ARCHIVE_SHA256))
	$(call gcc-fetch,$(GMP_DISTFILE),$(ZEDBSD_GMP_ARCHIVE_URL),$(ZEDBSD_GMP_ARCHIVE_SHA256))
	$(call gcc-fetch,$(MPFR_DISTFILE),$(ZEDBSD_MPFR_ARCHIVE_URL),$(ZEDBSD_MPFR_ARCHIVE_SHA256))
	$(call gcc-fetch,$(MPC_DISTFILE),$(ZEDBSD_MPC_ARCHIVE_URL),$(ZEDBSD_MPC_ARCHIVE_SHA256))
	rm -rf $(GCC_SOURCE).new
	mkdir -p $(GCC_SOURCE).new/gmp $(GCC_SOURCE).new/mpfr $(GCC_SOURCE).new/mpc
	tar -xf $(GCC_DISTFILE) --strip-components=1 -C $(GCC_SOURCE).new
	tar -xf $(GMP_DISTFILE) --strip-components=1 -C $(GCC_SOURCE).new/gmp
	tar -xf $(MPFR_DISTFILE) --strip-components=1 -C $(GCC_SOURCE).new/mpfr
	tar -xf $(MPC_DISTFILE) --strip-components=1 -C $(GCC_SOURCE).new/mpc
	for patch in $(GCC_PATCHES); do patch -d $(GCC_SOURCE).new -p1 < $$patch || exit 1; done
	echo '$(GCC_SOURCE_ID)' > $(GCC_SOURCE).new/.extracted
	rm -rf $(GCC_SOURCE)
	mv $(GCC_SOURCE).new $(GCC_SOURCE)

# The installed toolchain is named by its sources and its configuration:
# it is built again when either changes.  binutils is installed first, and
# GCC's configure finds the assembler and the linker in the same prefix.
GCC_BUILD_ID := $(BINUTILS_SOURCE_ID) $(GCC_SOURCE_ID) \
	$(BINUTILS_CONFIGURE_OPTIONS) $(GCC_CONFIGURE_OPTIONS) $(GCC_HOST_FLAGS)

# The toolchain is built again, from its sources, only when its sources
# or configuration changed: an installed toolchain that matches (built
# here, or extracted from the prebuilt archive) needs no sources.
ifneq ($(shell cat $(GCC_INSTALL)/.zedbsd-$(GCC_TARGET) 2>/dev/null),$(GCC_BUILD_ID))
$(GCC_INSTALL)/.zedbsd-$(GCC_TARGET): gcc-source-stale $(BINUTILS_SOURCE)/.extracted $(GCC_SOURCE)/.extracted
endif

$(GCC_INSTALL)/.zedbsd-$(GCC_TARGET):
	rm -rf $(GCC_BUILD)
	mkdir -p $(GCC_BUILD)/binutils $(GCC_BUILD)/gcc $(GCC_INSTALL)
	cd $(GCC_BUILD)/binutils && $(BINUTILS_SOURCE)/configure --prefix=$(GCC_INSTALL) $(BINUTILS_CONFIGURE_OPTIONS) $(GCC_HOST_FLAGS)
	$(GCC_SUBMAKE) -C $(GCC_BUILD)/binutils
	$(GCC_SUBMAKE) -C $(GCC_BUILD)/binutils install
	cd $(GCC_BUILD)/gcc && PATH='$(GCC_BIN)':"$$PATH" $(GCC_SOURCE)/configure --prefix=$(GCC_INSTALL) $(GCC_CONFIGURE_OPTIONS) $(GCC_HOST_FLAGS)
	$(GCC_SUBMAKE) -C $(GCC_BUILD)/gcc all-gcc all-target-libgcc
	$(GCC_SUBMAKE) -C $(GCC_BUILD)/gcc install-gcc install-target-libgcc
	echo '$(GCC_BUILD_ID)' > $@

#
# The prebuilt toolchain
#
# build/gcc of an x86_64 Linux host, as a release asset (version.mk).
# "toolchain-gcc-cached" fetches it, checks its SHA-256 and that it was
# built from the sources and configuration this tree names (its stamp),
# and puts it in place of build/gcc; "toolchain-gcc-cache-archive" makes
# the asset from this tree's build/gcc.
#

GCC_CACHE_URL := https://github.com/awemorris/zedBSD/releases/download/$(ZEDBSD_GCC_CACHE_TAG)/$(ZEDBSD_GCC_CACHE_ASSET)
GCC_CACHE_ARCHIVE := $(REPO_ROOT)/build/releases/$(ZEDBSD_GCC_CACHE_ASSET)
GCC_STAMP := $(GCC_INSTALL)/.zedbsd-$(GCC_TARGET)

.PHONY: toolchain-gcc-cached toolchain-gcc-cache-archive

toolchain-gcc-cached:
	@if [ "$$(cat '$(GCC_STAMP)' 2>/dev/null)" = '$(GCC_BUILD_ID)' ]; then \
		echo "GCC toolchain already installed at $(GCC_INSTALL)"; \
	else \
		mkdir -p '$(dir $(GCC_CACHE_ARCHIVE))' && \
		if [ ! -f '$(GCC_CACHE_ARCHIVE)' ]; then \
			echo "Downloading GCC cache $(ZEDBSD_GCC_CACHE_ASSET)..." && \
			curl --fail --location --output '$(GCC_CACHE_ARCHIVE).part' '$(GCC_CACHE_URL)' && \
			mv '$(GCC_CACHE_ARCHIVE).part' '$(GCC_CACHE_ARCHIVE)' || exit 1; \
		fi && \
		echo '$(ZEDBSD_GCC_CACHE_SHA256)  $(GCC_CACHE_ARCHIVE)' | sha256sum -c - && \
		echo "Extracting GCC cache..." && \
		rm -rf '$(GCC_INSTALL).new' && \
		mkdir -p '$(GCC_INSTALL).new' && \
		tar -xzf '$(GCC_CACHE_ARCHIVE)' --strip-components=1 -C '$(GCC_INSTALL).new' && \
		if [ "$$(cat '$(GCC_INSTALL).new/.zedbsd-$(GCC_TARGET)' 2>/dev/null)" != '$(GCC_BUILD_ID)' ]; then \
			echo "$(ZEDBSD_GCC_CACHE_ASSET) was not built from this tree's GCC sources and configuration" >&2; \
			exit 1; \
		fi && \
		rm -rf '$(GCC_INSTALL)' && \
		mv '$(GCC_INSTALL).new' '$(GCC_INSTALL)'; \
	fi
	@'$(GCC_BIN)/$(GCC_TARGET)-gcc' -dumpversion | grep -Fx '$(ZEDBSD_GCC_VERSION)' >/dev/null
	@'$(GCC_BIN)/$(GCC_TARGET)-ld' --version | grep -F '$(ZEDBSD_BINUTILS_VERSION)' >/dev/null

# The archive holds the directory gcc/ with ordered names, no owners and
# fixed times, compressed without a name or a time.
toolchain-gcc-cache-archive: toolchain-gcc
	@mkdir -p '$(dir $(GCC_CACHE_ARCHIVE))'
	tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 \
		-C '$(dir $(GCC_INSTALL))' -cf - '$(notdir $(GCC_INSTALL))' | \
		gzip -n -9 > '$(GCC_CACHE_ARCHIVE).part'
	mv '$(GCC_CACHE_ARCHIVE).part' '$(GCC_CACHE_ARCHIVE)'
	@sha256sum '$(GCC_CACHE_ARCHIVE)'
