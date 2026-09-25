# zedBSD userland package: OpenSSL (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# OpenSSL itself is Apache-2.0 and stays outside the source tree: only this
# Makefile, the patches beside it and the recorded archive identity live here.
# Provenance and the license audit are in plan/ws032/provenance.md.
#
# Registration as a selectable package and the cross build land with the build
# work; this file currently owns acquisition of the verified source tree.
ZEDBSD_OPENSSL_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_OPENSSL_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_OPENSSL_MAKEFILE)))
include $(ZEDBSD_OPENSSL_DIR)/../../../base/package.mk
include $(ZEDBSD_OPENSSL_DIR)/../../external.mk

# 3.5 is the current long-term support series; 3.5.8 was its latest release
# when this was recorded. Size and digest are the upstream bytes as measured,
# and the digest matches the .sha256 OpenSSL publishes beside the archive.
ZEDBSD_EXT_openssl_VERSION := 3.5.8
ZEDBSD_EXT_openssl_ROOT := openssl-3.5.8
ZEDBSD_EXT_openssl_ARCHIVE := openssl-3.5.8.tar.gz
ZEDBSD_EXT_openssl_URL := https://github.com/openssl/openssl/releases/download/openssl-3.5.8/openssl-3.5.8.tar.gz
ZEDBSD_EXT_openssl_SIZE := 53213818
ZEDBSD_EXT_openssl_SHA256 := a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2
ZEDBSD_EXT_openssl_PATCH_LEVEL := zedbsd6
ZEDBSD_EXT_openssl_PATCHES := $(sort $(wildcard $(abspath $(ZEDBSD_OPENSSL_DIR))/patches/*.patch))

# The Configure target name follows the configured architecture.
ZEDBSD_OPENSSL_TARGET := $(strip \
	$(if $(filter amd64,$(ZEDBSD_ARCHITECTURE)),zedbsd-x86_64,\
	$(if $(filter i386,$(ZEDBSD_ARCHITECTURE)),zedbsd-x86,)))

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,openssl))

# ---------------------------------------------------------------- build

# OpenSSL drives its own configure and Makefile, so the repository build waits
# on the staged files rather than on anything inside the external tree.
ZEDBSD_OPENSSL_CONFIGURED := $(ZEDBSD_EXT_openssl_BUILDDIR)/.zedbsd-configured
ZEDBSD_OPENSSL_STAGED := $(ZEDBSD_EXT_openssl_STAGEDIR)/.zedbsd-staged
ZEDBSD_OPENSSL_LIBCRYPTO := $(ZEDBSD_EXT_openssl_STAGEDIR)/usr/lib/libcrypto.so
ZEDBSD_OPENSSL_LIBSSL := $(ZEDBSD_EXT_openssl_STAGEDIR)/usr/lib/libssl.so
ZEDBSD_OPENSSL_APP := $(ZEDBSD_EXT_openssl_STAGEDIR)/usr/bin/openssl
ZEDBSD_OPENSSL_CONF := $(ZEDBSD_EXT_openssl_STAGEDIR)/etc/ssl/openssl.cnf
ZEDBSD_OPENSSL_LICENSE := $(ZEDBSD_EXT_openssl_SRCDIR)/LICENSE.txt

# no-asm: the x86_64 perlasm paths are not exercised yet.
# no-engine/no-dso/no-module: nothing loads code at run time here.
#
# The deprecated interfaces are kept.  They were left out while the zedBSD
# libc had no gethostbyname, which some of them reach for; the libc has it
# now, and OpenSSH is written against EC_KEY, which is among them.
# See plan/ws032/phase006/results.md for the rest.
ZEDBSD_OPENSSL_CONFIGURE_OPTIONS := \
	--prefix=/usr --openssldir=/etc/ssl --libdir=lib \
	no-asm no-tests no-docs no-engine no-dso no-module \
	shared threads

$(ZEDBSD_OPENSSL_CONFIGURED): $(ZEDBSD_EXT_openssl_SRCSTAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP)
	@rm -rf '$(ZEDBSD_EXT_openssl_BUILDDIR)'
	@mkdir -p '$(ZEDBSD_EXT_openssl_BUILDDIR)'
	cd '$(ZEDBSD_EXT_openssl_BUILDDIR)' && \
		. '$(ZEDBSD_EXTERNAL_CROSS_ENV)' && \
		perl '$(ZEDBSD_EXT_openssl_SRCDIR)/Configure' \
			$(ZEDBSD_OPENSSL_TARGET) \
			$(ZEDBSD_OPENSSL_CONFIGURE_OPTIONS) \
			CC="$$CC" AR="$$AR" RANLIB="$$RANLIB"
	@touch '$@'

$(ZEDBSD_OPENSSL_STAGED): $(ZEDBSD_OPENSSL_CONFIGURED)
	@rm -rf '$(ZEDBSD_EXT_openssl_STAGEDIR)'
	$(MAKE) -C '$(ZEDBSD_EXT_openssl_BUILDDIR)'
	$(MAKE) -C '$(ZEDBSD_EXT_openssl_BUILDDIR)' \
		DESTDIR='$(ZEDBSD_EXT_openssl_STAGEDIR)' install_sw install_ssldirs
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname libcrypto.so --needed libc.so '$(ZEDBSD_OPENSSL_LIBCRYPTO)'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname libssl.so --needed libcrypto.so --needed libc.so \
		'$(ZEDBSD_OPENSSL_LIBSSL)'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role application \
		--needed libssl.so --needed libcrypto.so --needed libc.so \
		'$(ZEDBSD_OPENSSL_APP)'
	@touch '$@'

$(ZEDBSD_OPENSSL_LIBCRYPTO) $(ZEDBSD_OPENSSL_LIBSSL) $(ZEDBSD_OPENSSL_APP) \
$(ZEDBSD_OPENSSL_CONF): $(ZEDBSD_OPENSSL_STAGED)
	@test -f '$@'

# The notice is taken from the extracted tree rather than the staged one, so
# it needs the source rather than the build.
$(ZEDBSD_OPENSSL_LICENSE): $(ZEDBSD_EXT_openssl_SRCSTAMP)
	@test -f '$@'

.PHONY: openssl
openssl: $(ZEDBSD_OPENSSL_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The shared objects go to /usr/lib like every package's; the runtime loader
# searches /lib (the base system) and then /usr/lib (packages).
$(eval $(call ZEDBSD_USERLAND_PACKAGE,openssl,OpenSSL cryptography,*,n,package,,y,\
	packages/security,security/openssl,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter openssl,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_OPENSSL_LIBCRYPTO) $(ZEDBSD_OPENSSL_LIBSSL) \
	$(ZEDBSD_OPENSSL_APP) $(ZEDBSD_OPENSSL_CONF) $(ZEDBSD_OPENSSL_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter openssl,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/libcrypto.so=$(ZEDBSD_OPENSSL_LIBCRYPTO) \
	--file /usr/lib/libssl.so=$(ZEDBSD_OPENSSL_LIBSSL) \
	--file $(ZEDBSD_PACKAGE_BINDIR)/openssl=$(ZEDBSD_OPENSSL_APP) \
	--file /etc/ssl/openssl.cnf=$(ZEDBSD_OPENSSL_CONF) \
	--file /usr/share/licenses/openssl/LICENSE.txt=$(ZEDBSD_OPENSSL_LICENSE))
