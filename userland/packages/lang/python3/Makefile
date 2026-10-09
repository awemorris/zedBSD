# zedBSD userland package: CPython 3 (external release tarball, WS126).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# CPython is PSF-2.0 licensed and stays outside the source tree: only this
# Makefile, the patches beside it and the recorded archive identity live here.
# Provenance and the license audit are in plan/ws126/provenance.md, the way it
# is built in plan/ws126/design.md.
#
# Registration as a selectable package, and the files an image takes, land
# with ws126-p005; this file owns acquisition, the build Python and the
# cross build of the interpreter into its stage (ws126-p002).
ZEDBSD_PYTHON3_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_PYTHON3_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_PYTHON3_MAKEFILE)))
include $(ZEDBSD_PYTHON3_DIR)/../../../base/package.mk
include $(ZEDBSD_PYTHON3_DIR)/../../external.mk

# 3.15.0 was not released yet when this was recorded (rc3 only), so the
# latest bugfix release of 3.14.  Size and digest are the upstream bytes as
# measured; the release manager's Sigstore signature was checked against them
# (plan/ws126/provenance.md).
ZEDBSD_EXT_python3_VERSION := 3.14.8
ZEDBSD_EXT_python3_ROOT := Python-3.14.8
ZEDBSD_EXT_python3_ARCHIVE := Python-3.14.8.tar.xz
ZEDBSD_EXT_python3_URL := https://www.python.org/ftp/python/3.14.8/Python-3.14.8.tar.xz
ZEDBSD_EXT_python3_SIZE := 24130644
ZEDBSD_EXT_python3_SHA256 := c2215904f02b175596dc49351585104f4bc20341e1c47378b26a2c274360ce73
ZEDBSD_EXT_python3_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_python3_PATCHES := $(sort $(wildcard $(abspath $(ZEDBSD_PYTHON3_DIR))/patches/*.patch))

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,python3))

# ---------------------------------------------------------------- build Python

# A cross build of CPython needs a build Python of the same version
# (--with-build-python), which the host's python3 is not.  It is built from
# the same tree for the host and installed beside the package's work tree,
# never into the host's /usr.
ZEDBSD_PYTHON3_HOST_BUILDDIR := $(ZEDBSD_EXT_python3_WORKDIR)/host-build
ZEDBSD_PYTHON3_HOST_PREFIX := $(ZEDBSD_EXT_python3_WORKDIR)/host
ZEDBSD_PYTHON3_HOST := $(ZEDBSD_PYTHON3_HOST_PREFIX)/bin/python3.14
ZEDBSD_PYTHON3_HOST_STAMP := $(ZEDBSD_PYTHON3_HOST_PREFIX)/.zedbsd-host-built

$(ZEDBSD_PYTHON3_HOST_STAMP): $(ZEDBSD_EXT_python3_SRCSTAMP)
	@rm -rf '$(ZEDBSD_PYTHON3_HOST_BUILDDIR)' '$(ZEDBSD_PYTHON3_HOST_PREFIX)'
	@mkdir -p '$(ZEDBSD_PYTHON3_HOST_BUILDDIR)'
	cd '$(ZEDBSD_PYTHON3_HOST_BUILDDIR)' && \
		'$(ZEDBSD_EXT_python3_SRCDIR)/configure' \
			--prefix='$(ZEDBSD_PYTHON3_HOST_PREFIX)' --without-ensurepip
	$(MAKE) -C '$(ZEDBSD_PYTHON3_HOST_BUILDDIR)'
	$(MAKE) -C '$(ZEDBSD_PYTHON3_HOST_BUILDDIR)' install
	@touch '$@'

# ---------------------------------------------------------------- target

# The staged zlib, named by its path rather than by the variables its
# makefile sets (as OpenSSH names OpenSSL's): this file may be read first.
ZEDBSD_PYTHON3_ZLIB_STAGE := $(ZEDBSD_EXTERNAL_WORKROOT)/zlib/stage
ZEDBSD_PYTHON3_ZLIB_STAMP := $(ZEDBSD_PYTHON3_ZLIB_STAGE)/.zedbsd-staged

ZEDBSD_PYTHON3_CONFIGURED := $(ZEDBSD_EXT_python3_BUILDDIR)/.zedbsd-configured
ZEDBSD_PYTHON3_STAGED := $(ZEDBSD_EXT_python3_STAGEDIR)/.zedbsd-staged
ZEDBSD_PYTHON3_SONAME := libpython3.14.so.1.0
ZEDBSD_PYTHON3_PROGRAM := $(ZEDBSD_EXT_python3_STAGEDIR)/usr/bin/python3.14
ZEDBSD_PYTHON3_LIBRARY := $(ZEDBSD_EXT_python3_STAGEDIR)/usr/lib/$(ZEDBSD_PYTHON3_SONAME)
ZEDBSD_PYTHON3_LIBDIR := $(ZEDBSD_EXT_python3_STAGEDIR)/usr/lib/python3.14
ZEDBSD_PYTHON3_LICENSE := $(ZEDBSD_EXT_python3_SRCDIR)/LICENSE

# The answers configure cannot find out in a cross build (plan/ws126/design.md):
# getaddrinfo works, IPv6 included; /dev/ptmx is there and the old BSD
# /dev/ptc is not.
#
# <netpacket/packet.h> exists, but zedBSD's packet sockets are addressed by
# struct sockaddr_l2, not by the Linux struct sockaddr_ll (and PACKET_HOST)
# that the socket module writes its AF_PACKET support against.  configure is
# told the header is absent, so the module is built without AF_PACKET.
ZEDBSD_PYTHON3_CACHE := \
	ac_cv_buggy_getaddrinfo=no \
	ac_cv_file__dev_ptmx=yes \
	ac_cv_file__dev_ptc=no \
	ac_cv_header_netpacket_packet_h=no

# pkg-config sees only what was staged for zedBSD: the host's own libraries
# (libffi, liblzma, zstd) would otherwise be taken for the target's.
ZEDBSD_PYTHON3_PKG_CONFIG := \
	PKG_CONFIG_PATH= \
	PKG_CONFIG_LIBDIR='$(ZEDBSD_PYTHON3_ZLIB_STAGE)/usr/lib/pkgconfig' \
	PKG_CONFIG_SYSROOT_DIR='$(ZEDBSD_PYTHON3_ZLIB_STAGE)'

# --enable-shared: libpython and the extension modules as shared objects.
# --without-ensurepip: pip is ws126-p005's decision (D2).
# --without-mimalloc: its Unix layer reaches for <sys/syscall.h> and raw
#   system calls, which zedBSD does not offer; the default build (with the GIL)
#   allocates with pymalloc over the C library's malloc instead.
# --with-build-python: the build Python above.
$(ZEDBSD_PYTHON3_CONFIGURED): $(ZEDBSD_EXT_python3_SRCSTAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_PYTHON3_HOST_STAMP) \
	$(ZEDBSD_PYTHON3_ZLIB_STAMP) $(ZEDBSD_PYTHON3_MAKEFILE)
	@rm -rf '$(ZEDBSD_EXT_python3_BUILDDIR)'
	@mkdir -p '$(ZEDBSD_EXT_python3_BUILDDIR)'
	cd '$(ZEDBSD_EXT_python3_BUILDDIR)' && \
		. '$(ZEDBSD_EXTERNAL_CROSS_ENV)' && \
		$(ZEDBSD_PYTHON3_PKG_CONFIG) \
		CONFIG_SITE="$$ZEDBSD_CROSS_AUTOCONF_CACHE" \
		'$(ZEDBSD_EXT_python3_SRCDIR)/configure' \
			--host="$$ZEDBSD_CROSS_TRIPLE" \
			--build="$$('$(ZEDBSD_EXT_python3_SRCDIR)/config.guess')" \
			--with-build-python='$(ZEDBSD_PYTHON3_HOST)' \
			--prefix=/usr --enable-shared --without-ensurepip --without-mimalloc \
			$(ZEDBSD_PYTHON3_CACHE) \
			CC="$$CC" CXX="$$CXX" AR="$$AR" RANLIB="$$RANLIB" READELF="$$READELF" \
			LDFLAGS="-Wl,-rpath-link,$(ZEDBSD_PYTHON3_ZLIB_STAGE)/usr/lib"
	@touch '$@'

$(ZEDBSD_PYTHON3_STAGED): $(ZEDBSD_PYTHON3_CONFIGURED)
	@rm -rf '$(ZEDBSD_EXT_python3_STAGEDIR)'
	$(MAKE) -C '$(ZEDBSD_EXT_python3_BUILDDIR)'
	$(MAKE) -C '$(ZEDBSD_EXT_python3_BUILDDIR)' \
		DESTDIR='$(ZEDBSD_EXT_python3_STAGEDIR)' install
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_PYTHON3_SONAME) --needed libc.so \
		'$(ZEDBSD_PYTHON3_LIBRARY)'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role application \
		--needed $(ZEDBSD_PYTHON3_SONAME) --needed libc.so \
		'$(ZEDBSD_PYTHON3_PROGRAM)'
	@touch '$@'

$(ZEDBSD_PYTHON3_PROGRAM) $(ZEDBSD_PYTHON3_LIBRARY): $(ZEDBSD_PYTHON3_STAGED)
	@test -f '$@'

# The notice is taken from the extracted tree rather than the staged one.
$(ZEDBSD_PYTHON3_LICENSE): $(ZEDBSD_EXT_python3_SRCSTAMP)
	@test -f '$@'

.PHONY: python3 python3-host
python3-host: $(ZEDBSD_PYTHON3_HOST_STAMP)
	@:
python3: $(ZEDBSD_PYTHON3_STAGED)
	@:
