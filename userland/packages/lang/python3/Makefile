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
PYTHON3_MAKEFILE := $(lastword $(MAKEFILE_LIST))
PYTHON3_DIR := $(patsubst %/,%,$(dir $(PYTHON3_MAKEFILE)))
include $(PYTHON3_DIR)/../../../base/package.mk
include $(PYTHON3_DIR)/../../external.mk

# 3.15.0 was not released yet when this was recorded (rc3 only), so the
# latest bugfix release of 3.14.  Size and digest are the upstream bytes as
# measured; the release manager's Sigstore signature was checked against them
# (plan/ws126/provenance.md).
EXT_python3_VERSION := 3.14.8
EXT_python3_ROOT := Python-3.14.8
EXT_python3_ARCHIVE := Python-3.14.8.tar.xz
EXT_python3_URL := https://www.python.org/ftp/python/3.14.8/Python-3.14.8.tar.xz
EXT_python3_SIZE := 24130644
EXT_python3_SHA256 := c2215904f02b175596dc49351585104f4bc20341e1c47378b26a2c274360ce73
EXT_python3_PATCH_LEVEL := zedbsd1
EXT_python3_PATCHES := $(sort $(wildcard $(abspath $(PYTHON3_DIR))/patches/*.patch))

$(eval $(call EXTERNAL_SOURCE,python3))

# ---------------------------------------------------------------- build Python

# A cross build of CPython needs a build Python of the same version
# (--with-build-python), which the host's python3 is not.  It is built from
# the same tree for the host and installed beside the package's work tree,
# never into the host's /usr.
PYTHON3_HOST_BUILDDIR := $(EXT_python3_WORKDIR)/host-build
PYTHON3_HOST_PREFIX := $(EXT_python3_WORKDIR)/host
PYTHON3_HOST := $(PYTHON3_HOST_PREFIX)/bin/python3.14
PYTHON3_HOST_STAMP := $(PYTHON3_HOST_PREFIX)/.zedbsd-host-built

$(PYTHON3_HOST_STAMP): $(EXT_python3_SRCSTAMP)
	@rm -rf '$(PYTHON3_HOST_BUILDDIR)' '$(PYTHON3_HOST_PREFIX)'
	@mkdir -p '$(PYTHON3_HOST_BUILDDIR)'
	cd '$(PYTHON3_HOST_BUILDDIR)' && \
		'$(EXT_python3_SRCDIR)/configure' \
			--prefix='$(PYTHON3_HOST_PREFIX)' --without-ensurepip
	$(MAKE) -C '$(PYTHON3_HOST_BUILDDIR)'
	$(MAKE) -C '$(PYTHON3_HOST_BUILDDIR)' install
	@touch '$@'

# ---------------------------------------------------------------- target

# The staged zlib, named by its path rather than by the variables its
# makefile sets (as OpenSSH names OpenSSL's): this file may be read first.
PYTHON3_ZLIB_STAGE := $(EXTERNAL_WORKROOT)/zlib/stage
PYTHON3_ZLIB_STAMP := $(PYTHON3_ZLIB_STAGE)/.zedbsd-staged

# The staged OpenSSL, named by path for the same reason, for the ssl and
# hashlib modules (ws126-p003).  configure is given its directory rather than
# its .pc files: pkg-config here prefixes one stage (zlib's) as the sysroot,
# and OpenSSL's stage is another.  The default CA bundle is OpenSSL's own
# compiled-in default, /etc/ssl/cert.pem (--openssldir=/etc/ssl), which is
# where security/ca-certificates installs it, so ssl.get_default_verify_paths()
# reports it with nothing set here.
PYTHON3_SSL_STAGE := $(EXTERNAL_WORKROOT)/openssl/stage
PYTHON3_SSL_STAMP := $(PYTHON3_SSL_STAGE)/.zedbsd-staged

PYTHON3_CONFIGURED := $(EXT_python3_BUILDDIR)/.zedbsd-configured
PYTHON3_STAGED := $(EXT_python3_STAGEDIR)/.zedbsd-staged
PYTHON3_SONAME := libpython3.14.so.1.0
PYTHON3_PROGRAM := $(EXT_python3_STAGEDIR)/usr/bin/python3.14
PYTHON3_LIBRARY := $(EXT_python3_STAGEDIR)/usr/lib/$(PYTHON3_SONAME)
PYTHON3_LIBDIR := $(EXT_python3_STAGEDIR)/usr/lib/python3.14
PYTHON3_LICENSE := $(EXT_python3_SRCDIR)/LICENSE

# The answers configure cannot find out in a cross build (plan/ws126/design.md):
# getaddrinfo works, IPv6 included; /dev/ptmx is there and the old BSD
# /dev/ptc is not.
#
# <netpacket/packet.h> exists, but zedBSD's packet sockets are addressed by
# struct sockaddr_l2, not by the Linux struct sockaddr_ll (and PACKET_HOST)
# that the socket module writes its AF_PACKET support against.  configure is
# told the header is absent, so the module is built without AF_PACKET.
PYTHON3_CACHE := \
	ac_cv_buggy_getaddrinfo=no \
	ac_cv_file__dev_ptmx=yes \
	ac_cv_file__dev_ptc=no \
	ac_cv_header_netpacket_packet_h=no

# pkg-config sees only what was staged for zedBSD: the host's own libraries
# (libffi, liblzma, zstd) would otherwise be taken for the target's.
PYTHON3_PKG_CONFIG := \
	PKG_CONFIG_PATH= \
	PKG_CONFIG_LIBDIR='$(PYTHON3_ZLIB_STAGE)/usr/lib/pkgconfig' \
	PKG_CONFIG_SYSROOT_DIR='$(PYTHON3_ZLIB_STAGE)'

# --enable-shared: libpython and the extension modules as shared objects.
# --without-ensurepip: pip is ws126-p005's decision (D2).
# --without-mimalloc: its Unix layer reaches for <sys/syscall.h> and raw
#   system calls, which zedBSD does not offer; the default build (with the GIL)
#   allocates with pymalloc over the C library's malloc instead.
# --with-build-python: the build Python above.
# --with-openssl: the staged OpenSSL above; --with-openssl-rpath=no keeps its
#   stage directory out of the modules, which find libssl.so and
#   libcrypto.so through the ordinary /lib lookup.
$(PYTHON3_CONFIGURED): $(EXT_python3_SRCSTAMP) \
	$(EXTERNAL_CROSS_STAMP) $(PYTHON3_HOST_STAMP) \
	$(PYTHON3_ZLIB_STAMP) $(PYTHON3_SSL_STAMP) \
	$(PYTHON3_MAKEFILE)
	@rm -rf '$(EXT_python3_BUILDDIR)'
	@mkdir -p '$(EXT_python3_BUILDDIR)'
	cd '$(EXT_python3_BUILDDIR)' && \
		. '$(EXTERNAL_CROSS_ENV)' && \
		$(PYTHON3_PKG_CONFIG) \
		CONFIG_SITE="$$CROSS_AUTOCONF_CACHE" \
		'$(EXT_python3_SRCDIR)/configure' \
			--host="$$CROSS_TRIPLE" \
			--build="$$('$(EXT_python3_SRCDIR)/config.guess')" \
			--with-build-python='$(PYTHON3_HOST)' \
			--prefix=/usr --enable-shared --without-ensurepip --without-mimalloc \
			--with-openssl='$(PYTHON3_SSL_STAGE)/usr' --with-openssl-rpath=no \
			$(PYTHON3_CACHE) \
			CC="$$CC" CXX="$$CXX" AR="$$AR" RANLIB="$$RANLIB" READELF="$$READELF" \
			LDFLAGS="-Wl,-rpath-link,$(PYTHON3_ZLIB_STAGE)/usr/lib -Wl,-rpath-link,$(PYTHON3_SSL_STAGE)/usr/lib"
	@touch '$@'

$(PYTHON3_STAGED): $(PYTHON3_CONFIGURED)
	@rm -rf '$(EXT_python3_STAGEDIR)'
	$(MAKE) -C '$(EXT_python3_BUILDDIR)'
	$(MAKE) -C '$(EXT_python3_BUILDDIR)' \
		DESTDIR='$(EXT_python3_STAGEDIR)' install
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(PYTHON3_SONAME) --needed libc.so \
		'$(PYTHON3_LIBRARY)'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role application \
		--needed $(PYTHON3_SONAME) --needed libc.so \
		'$(PYTHON3_PROGRAM)'
	@touch '$@'

$(PYTHON3_PROGRAM) $(PYTHON3_LIBRARY): $(PYTHON3_STAGED)
	@test -f '$@'

# The notice is taken from the extracted tree rather than the staged one.
$(PYTHON3_LICENSE): $(EXT_python3_SRCSTAMP)
	@test -f '$@'

.PHONY: python3 python3-host
python3-host: $(PYTHON3_HOST_STAMP)
	@:
python3: $(PYTHON3_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# What the image carries (ws126-p005): the interpreter and libpython,
# stripped of their debugging sections, and the standard library as one
# directory (--subtree, ws125-p002).  The library goes in without the test
# suite (test/, about 160 MB), the Tk and IDLE parts (tkinter, idlelib,
# turtledemo: no Tk here), ensurepip (pip comes later, 2026-10-02 user), the
# build's own configuration directory (config-3.14, the static libpython for
# embedding), the test-only extension modules (_test*, xx*), and the
# optimized bytecode (.opt-1, .opt-2: the ordinary .pyc is kept, so nothing
# is compiled on first use).  The extension modules are stripped too.
PYTHON3_IMAGE := $(EXT_python3_WORKDIR)/image
PYTHON3_IMAGE_STAMP := $(PYTHON3_IMAGE)/.zedbsd-image
PYTHON3_IMAGE_LIBDIR := $(PYTHON3_IMAGE)/lib/python3.14
PYTHON3_IMAGE_PROGRAM := $(PYTHON3_IMAGE)/bin/python3.14
PYTHON3_IMAGE_LIBRARY := $(PYTHON3_IMAGE)/lib/$(PYTHON3_SONAME)
PYTHON3_IMAGE_EXCLUDE := \
	--exclude=./test --exclude=./idlelib --exclude=./tkinter \
	--exclude=./turtledemo --exclude=./ensurepip --exclude=./config-3.14 \
	--exclude='./lib-dynload/_test*' --exclude='./lib-dynload/xx*' \
	--exclude='*.opt-1.pyc' --exclude='*.opt-2.pyc'

$(PYTHON3_IMAGE_STAMP): $(PYTHON3_STAGED) $(PYTHON3_MAKEFILE)
	@rm -rf '$(PYTHON3_IMAGE)'
	@mkdir -p '$(PYTHON3_IMAGE_LIBDIR)' '$(PYTHON3_IMAGE)/bin'
	tar -C '$(PYTHON3_LIBDIR)' $(PYTHON3_IMAGE_EXCLUDE) -cf - . | \
		tar -C '$(PYTHON3_IMAGE_LIBDIR)' -xpf -
	cp -p '$(PYTHON3_PROGRAM)' '$(PYTHON3_IMAGE_PROGRAM)'
	cp -p '$(PYTHON3_LIBRARY)' '$(PYTHON3_IMAGE_LIBRARY)'
	. '$(EXTERNAL_CROSS_ENV)' && "$$STRIP" --strip-unneeded \
		'$(PYTHON3_IMAGE_PROGRAM)' '$(PYTHON3_IMAGE_LIBRARY)' \
		'$(PYTHON3_IMAGE_LIBDIR)'/lib-dynload/*.so
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(PYTHON3_SONAME) --needed libc.so \
		'$(PYTHON3_IMAGE_LIBRARY)'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role application \
		--needed $(PYTHON3_SONAME) --needed libc.so \
		'$(PYTHON3_IMAGE_PROGRAM)'
	@touch '$@'

$(PYTHON3_IMAGE_PROGRAM) $(PYTHON3_IMAGE_LIBRARY): $(PYTHON3_IMAGE_STAMP)
	@test -f '$@'

.PHONY: python3-image
python3-image: $(PYTHON3_IMAGE_STAMP)
	@:

# Choosing python3 chooses what its modules are built against: zlib, OpenSSL
# (ssl, hashlib) and the CA bundle ssl reads by default.  Off by default
# (D3, as vim and Emacs).
$(eval $(call USERLAND_PACKAGE,python3,Python 3 interpreter,*,n,package,,y,\
	packages/lang,lang/python3,libs/zlib security/openssl security/ca-certificates))

PACKAGE_INPUTS += $(if $(filter python3,$(USER_PROGRAMS)),\
	$(PYTHON3_IMAGE_STAMP) $(PYTHON3_LICENSE))
PACKAGE_FILES += $(if $(filter python3,$(USER_PROGRAMS)),\
	--file $(PACKAGE_BINDIR)/python3.14=$(PYTHON3_IMAGE_PROGRAM) \
	--file /usr/lib/$(PYTHON3_SONAME)=$(PYTHON3_IMAGE_LIBRARY) \
	--subtree /usr/lib/python3.14=$(PYTHON3_IMAGE_LIBDIR) \
	--file /usr/share/licenses/python3/LICENSE=$(PYTHON3_LICENSE))
PACKAGE_LINKS += $(if $(filter python3,$(USER_PROGRAMS)),\
	$(PACKAGE_BINDIR)/python3=python3.14)
