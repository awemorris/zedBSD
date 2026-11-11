# zedBSD build-machine tool: gperf (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# gperf generates perfect hash functions as C source.  fontconfig runs it
# while it builds (its meson.build requires the program), and the build
# machine does not have it, so it is built here from the release, for the
# build machine, with the build machine's compiler.  It is a build tool
# only: it is not a userland package, is never offered by the menu, and
# nothing of it goes into an image.  gperf is GPL-3.0-or-later; the source
# stays outside the tree, and only this Makefile and the recorded archive
# identity live here.  Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
GPERF_MAKEFILE := $(lastword $(MAKEFILE_LIST))
GPERF_DIR := $(patsubst %/,%,$(dir $(GPERF_MAKEFILE)))
include $(GPERF_DIR)/../../../base/package.mk
include $(GPERF_DIR)/../../external.mk

# 3.3 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked against
# the GNU keyring (WS034 p001).  Fetched from the kernel.org GNU mirror: ftp.gnu.org
# did not answer (2026-10-06, CI and the build host); the digest is the same.
EXT_gperf_VERSION := 3.3
EXT_gperf_ROOT := gperf-3.3
EXT_gperf_ARCHIVE := gperf-3.3.tar.gz
EXT_gperf_URL := https://mirrors.kernel.org/gnu/gperf/gperf-3.3.tar.gz
EXT_gperf_SIZE := 1831294
EXT_gperf_SHA256 := fd87e0aba7e43ae054837afd6cd4db03a3f2693deb3619085e6ed9d8d9604ad8
EXT_gperf_PATCH_LEVEL := zedbsd0
EXT_gperf_PATCHES :=

$(eval $(call EXTERNAL_SOURCE,gperf))

# ---------------------------------------------------------------- build

# The build machine's compiler, not the cross wrappers: the program runs
# during the build.  configure is given cc and c++ by name so that a CC or
# CXX set for a cross build in the caller's environment is not picked up.
GPERF_CONFIGURED := $(EXT_gperf_BUILDDIR)/.zedbsd-configured
GPERF_STAGED := $(EXT_gperf_STAGEDIR)/.zedbsd-staged
GPERF_BINDIR := $(EXT_gperf_STAGEDIR)/usr/bin
GPERF_HOST := $(GPERF_BINDIR)/gperf

# The source tree is configured out of tree so that it stays as extracted.
$(GPERF_CONFIGURED): $(EXT_gperf_SRCSTAMP) $(GPERF_MAKEFILE)
	@rm -rf '$(EXT_gperf_BUILDDIR)'
	@mkdir -p '$(EXT_gperf_BUILDDIR)'
	cd '$(EXT_gperf_BUILDDIR)' && env -u CPPFLAGS -u CFLAGS -u CXXFLAGS -u LDFLAGS \
		CC=cc CXX=c++ '$(EXT_gperf_SRCDIR)/configure' --prefix=/usr --quiet
	@touch '$@'

# gperf's own Makefiles are run without this build's command-line
# variables (BUILD, CC, ...), which mean something else to them.
GPERF_MAKE := env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make

# The program is run once after the install, so a build that produced
# something that cannot run on this machine fails here rather than inside
# the first package that uses it.
$(GPERF_STAGED): $(GPERF_CONFIGURED)
	@rm -rf '$(EXT_gperf_STAGEDIR)'
	$(GPERF_MAKE) -C '$(EXT_gperf_BUILDDIR)' -j $(BUILD_JOBS)
	$(GPERF_MAKE) -C '$(EXT_gperf_BUILDDIR)' install \
		DESTDIR='$(EXT_gperf_STAGEDIR)'
	'$(GPERF_HOST)' --version
	@touch '$@'

$(GPERF_HOST): $(GPERF_STAGED)
	@test -x '$@'

.PHONY: gperf-host
gperf-host: $(GPERF_STAGED)
	@:

# Meson builds find it on PATH; a package that runs it names
# $(GPERF_HOST) in its EXT_<name>_HOST_TOOLS.
EXTERNAL_HOST_BIN_DIRS += $(GPERF_BINDIR)
