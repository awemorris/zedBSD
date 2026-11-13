# zedBSD userland package: OpenSSL (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# An external package, never part of the system: it is built and put in an
# image only when the configuration names the package openssl (curl,
# python3 and openssh need it, so a configuration that names them names it
# too).  Nothing of the system needs it; the system's own TLS and
# cryptography are libssl-compat and libcrypto-compat (userland/base/),
# which the browser, Mail and ssh use.
#
# OpenSSL itself is Apache-2.0 and stays outside the source tree: only this
# Makefile, the patches beside it and the recorded archive identity live here.
OPENSSL_MAKEFILE := $(lastword $(MAKEFILE_LIST))
OPENSSL_DIR := $(patsubst %/,%,$(dir $(OPENSSL_MAKEFILE)))
include $(OPENSSL_DIR)/../../../base/package.mk
include $(OPENSSL_DIR)/../../external.mk

# 3.5 is the current long-term support series; 3.5.8 was its latest release
# when this was recorded. Size and digest are the upstream bytes as measured,
# and the digest matches the .sha256 OpenSSL publishes beside the archive.
EXT_openssl_VERSION := 3.5.8
EXT_openssl_ROOT := openssl-3.5.8
EXT_openssl_ARCHIVE := openssl-3.5.8.tar.gz
EXT_openssl_URL := https://github.com/openssl/openssl/releases/download/openssl-3.5.8/openssl-3.5.8.tar.gz
EXT_openssl_SIZE := 53213818
EXT_openssl_SHA256 := a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2
EXT_openssl_PATCH_LEVEL := zedbsd8
EXT_openssl_PATCHES := $(sort $(wildcard $(abspath $(OPENSSL_DIR))/patches/*.patch))

# The Configure target name follows the configured architecture.
OPENSSL_TARGET := $(strip \
	$(if $(filter x86_64,$(ARCHITECTURE)),zedbsd-x86_64,\
	$(if $(filter i386,$(ARCHITECTURE)),zedbsd-x86,\
	$(if $(filter arm64,$(ARCHITECTURE)),zedbsd-aarch64,\
	$(if $(filter riscv64,$(ARCHITECTURE)),zedbsd-riscv64,\
	$(if $(filter armv7,$(ARCHITECTURE)),zedbsd-armv7,))))))

$(eval $(call EXTERNAL_SOURCE,openssl))

# ---------------------------------------------------------------- build

# OpenSSL drives its own configure and Makefile, so the repository build waits
# on the staged files rather than on anything inside the external tree.
OPENSSL_CONFIGURED := $(EXT_openssl_BUILDDIR)/.zedbsd-configured
OPENSSL_STAGED := $(EXT_openssl_STAGEDIR)/.zedbsd-staged
OPENSSL_LIBCRYPTO := $(EXT_openssl_STAGEDIR)/usr/lib/libcrypto.so
OPENSSL_LIBSSL := $(EXT_openssl_STAGEDIR)/usr/lib/libssl.so
OPENSSL_APP := $(EXT_openssl_STAGEDIR)/usr/bin/openssl
OPENSSL_CONF := $(EXT_openssl_STAGEDIR)/etc/ssl/openssl.cnf
OPENSSL_LICENSE := $(EXT_openssl_SRCDIR)/LICENSE.txt

# no-asm: the x86_64 perlasm paths are not exercised yet.
# no-engine/no-dso/no-module: nothing loads code at run time here.
#
# The deprecated interfaces are kept.  They were left out while the zedBSD
# libc had no gethostbyname, which some of them reach for; the libc has it
# now, and OpenSSH is written against EC_KEY, which is among them.
OPENSSL_CONFIGURE_OPTIONS := \
	--prefix=/usr --openssldir=/etc/ssl --libdir=lib \
	no-asm no-tests no-docs no-engine no-dso no-module \
	shared threads

$(OPENSSL_CONFIGURED): $(EXT_openssl_SRCSTAMP) \
	$(EXTERNAL_CROSS_STAMP)
	@rm -rf '$(EXT_openssl_BUILDDIR)'
	@mkdir -p '$(EXT_openssl_BUILDDIR)'
	cd '$(EXT_openssl_BUILDDIR)' && \
		. '$(EXTERNAL_CROSS_ENV)' && \
		perl '$(EXT_openssl_SRCDIR)/Configure' \
			$(OPENSSL_TARGET) \
			$(OPENSSL_CONFIGURE_OPTIONS) \
			CC="$$CC" AR="$$AR" RANLIB="$$RANLIB"
	@touch '$@'

$(OPENSSL_STAGED): $(OPENSSL_CONFIGURED)
	@rm -rf '$(EXT_openssl_STAGEDIR)'
	$(MAKE) -C '$(EXT_openssl_BUILDDIR)'
	$(MAKE) -C '$(EXT_openssl_BUILDDIR)' \
		DESTDIR='$(EXT_openssl_STAGEDIR)' install_sw install_ssldirs
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname libcrypto.so --needed libc.so '$(OPENSSL_LIBCRYPTO)'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname libssl.so --needed libcrypto.so --needed libc.so \
		'$(OPENSSL_LIBSSL)'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role application \
		--needed libssl.so --needed libcrypto.so --needed libc.so \
		'$(OPENSSL_APP)'
	@touch '$@'

$(OPENSSL_LIBCRYPTO) $(OPENSSL_LIBSSL) $(OPENSSL_APP) \
$(OPENSSL_CONF): $(OPENSSL_STAGED)
	@test -f '$@'

# The notice is taken from the extracted tree rather than the staged one, so
# it needs the source rather than the build.
$(OPENSSL_LICENSE): $(EXT_openssl_SRCSTAMP)
	@test -f '$@'

.PHONY: openssl
openssl: $(OPENSSL_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The shared objects go to /usr/lib like every package's; the runtime loader
# searches /lib (the base system) and then /usr/lib (packages).
$(eval $(call USERLAND_PACKAGE,openssl,OpenSSL cryptography,*,n,package,,y,\
	packages/security,security/openssl,))

PACKAGE_INPUTS += $(if $(filter openssl,$(USER_PROGRAMS)),\
	$(OPENSSL_LIBCRYPTO) $(OPENSSL_LIBSSL) \
	$(OPENSSL_APP) $(OPENSSL_CONF) $(OPENSSL_LICENSE))
PACKAGE_FILES += $(if $(filter openssl,$(USER_PROGRAMS)),\
	--file /usr/lib/libcrypto.so=$(OPENSSL_LIBCRYPTO) \
	--file /usr/lib/libssl.so=$(OPENSSL_LIBSSL) \
	--file $(PACKAGE_BINDIR)/openssl=$(OPENSSL_APP) \
	--file /etc/ssl/openssl.cnf=$(OPENSSL_CONF) \
	--file /usr/share/licenses/openssl/LICENSE.txt=$(OPENSSL_LICENSE))

# The development files: the headers and the .pc files.  The libraries are
# installed under their unversioned SONAMEs, so they need no links.
$(eval $(call EXTERNAL_DEVELOPMENT,openssl,$(EXT_openssl_STAGEDIR),\
	$(addprefix lib/pkgconfig/,libcrypto.pc libssl.pc openssl.pc),\
	include/openssl,\
	))
