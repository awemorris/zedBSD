# zedBSD userland package: curl and libcurl (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# curl is under its own MIT-style licence and stays outside the source tree:
# only this Makefile and the recorded archive identity live here.
# Provenance is in plan/ws034/package-inventory.md.
ZEDBSD_CURL_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_CURL_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_CURL_MAKEFILE)))
include $(ZEDBSD_CURL_DIR)/../../../base/package.mk
include $(ZEDBSD_CURL_DIR)/../../external.mk

# 8.22.0 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked with the
# maintainer's key (WS034 p001).
ZEDBSD_EXT_curl_VERSION := 8.22.0
ZEDBSD_EXT_curl_ROOT := curl-8.22.0
ZEDBSD_EXT_curl_ARCHIVE := curl-8.22.0.tar.xz
ZEDBSD_EXT_curl_URL := https://curl.se/download/curl-8.22.0.tar.xz
ZEDBSD_EXT_curl_SIZE := 2953092
ZEDBSD_EXT_curl_SHA256 := f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
ZEDBSD_EXT_curl_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_curl_PATCHES :=

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,curl))

# ---------------------------------------------------------------- build

ZEDBSD_CURL_SONAME := libcurl.so.4
ZEDBSD_CURL_CONFIGURED := $(ZEDBSD_EXT_curl_BUILDDIR)/.zedbsd-configured
ZEDBSD_CURL_STAGED := $(ZEDBSD_EXT_curl_STAGEDIR)/.zedbsd-staged
ZEDBSD_CURL_LIBRARY := $(ZEDBSD_EXT_curl_STAGEDIR)/usr/lib/libcurl.so.4.8.0
ZEDBSD_CURL_PROGRAM := $(ZEDBSD_EXT_curl_STAGEDIR)/usr/bin/curl
ZEDBSD_CURL_PC := $(ZEDBSD_EXT_curl_STAGEDIR)/usr/lib/pkgconfig/libcurl.pc
ZEDBSD_CURL_LICENSE := $(ZEDBSD_EXT_curl_SRCDIR)/COPYING

# The staged OpenSSL and zlib are named by path: this file is read before
# theirs, so their variables would still be empty here.  They are not in the
# sysroot, where the toolchain file confines CMake's searches, so each
# header directory and library is given to CMake directly.
ZEDBSD_CURL_SSL_STAGE := $(ZEDBSD_EXTERNAL_WORKROOT)/openssl/stage/usr
ZEDBSD_CURL_Z_STAGE := $(ZEDBSD_EXTERNAL_WORKROOT)/zlib/stage/usr
ZEDBSD_CURL_DEPENDENCIES := \
	$(ZEDBSD_EXTERNAL_WORKROOT)/openssl/stage/.zedbsd-staged \
	$(ZEDBSD_EXTERNAL_WORKROOT)/zlib/stage/.zedbsd-staged

# OpenSSL for TLS and zlib for compressed transfers.  libpsl, libidn2,
# libssh2, nghttp2, brotli, zstd, LDAP and GSSAPI are not built here and are
# turned off rather than left to be looked for.  The CA bundle is the one the
# ca-certificates package installs (WS034 p019); cross-compiling, CMake
# cannot find it by itself.  The manual pages need perl-built tools and are
# not made.  Symbol versioning stays off: the loader does not read it.
ZEDBSD_CURL_CMAKE_OPTIONS := \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DBUILD_SHARED_LIBS=ON \
	-DBUILD_STATIC_LIBS=OFF \
	-DBUILD_CURL_EXE=ON \
	-DBUILD_TESTING=OFF \
	-DBUILD_EXAMPLES=OFF \
	-DBUILD_LIBCURL_DOCS=OFF \
	-DBUILD_MISC_DOCS=OFF \
	-DENABLE_CURL_MANUAL=OFF \
	-DCURL_LIBCURL_SOVERSION=ON \
	-DCURL_LIBCURL_VERSIONED_SYMBOLS=OFF \
	-DCURL_USE_PKGCONFIG=OFF \
	-DCURL_USE_OPENSSL=ON \
	-DOPENSSL_ROOT_DIR='$(ZEDBSD_CURL_SSL_STAGE)' \
	-DOPENSSL_INCLUDE_DIR='$(ZEDBSD_CURL_SSL_STAGE)/include' \
	-DOPENSSL_SSL_LIBRARY='$(ZEDBSD_CURL_SSL_STAGE)/lib/libssl.so' \
	-DOPENSSL_CRYPTO_LIBRARY='$(ZEDBSD_CURL_SSL_STAGE)/lib/libcrypto.so' \
	-DCURL_ZLIB=ON \
	-DZLIB_INCLUDE_DIR='$(ZEDBSD_CURL_Z_STAGE)/include' \
	-DZLIB_LIBRARY='$(ZEDBSD_CURL_Z_STAGE)/lib/libz.so' \
	-DCURL_USE_LIBPSL=OFF \
	-DUSE_LIBIDN2=OFF \
	-DCURL_USE_LIBSSH2=OFF \
	-DUSE_NGHTTP2=OFF \
	-DCURL_BROTLI=OFF \
	-DCURL_ZSTD=OFF \
	-DCURL_DISABLE_LDAP=ON \
	-DCURL_DISABLE_LDAPS=ON \
	-DCURL_USE_GSSAPI=OFF \
	-DCURL_CA_BUNDLE=/etc/ssl/cert.pem \
	-DCURL_CA_PATH=none \
	-DCMAKE_SHARED_LINKER_FLAGS='-Wl,-rpath-link,$(ZEDBSD_CURL_SSL_STAGE)/lib -Wl,-rpath-link,$(ZEDBSD_CURL_Z_STAGE)/lib' \
	-DCMAKE_EXE_LINKER_FLAGS='-Wl,-rpath-link,$(ZEDBSD_CURL_SSL_STAGE)/lib -Wl,-rpath-link,$(ZEDBSD_CURL_Z_STAGE)/lib'

# The options are read when the tree is configured, so this file is an input.
$(ZEDBSD_CURL_CONFIGURED): $(ZEDBSD_EXT_curl_SRCSTAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_CURL_DEPENDENCIES) \
	$(ZEDBSD_CURL_MAKEFILE)
	@rm -rf '$(ZEDBSD_EXT_curl_BUILDDIR)'
	@mkdir -p '$(ZEDBSD_EXT_curl_BUILDDIR)'
	cmake -S '$(ZEDBSD_EXT_curl_SRCDIR)' -B '$(ZEDBSD_EXT_curl_BUILDDIR)' \
		-G Ninja \
		--toolchain '$(ZEDBSD_EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(ZEDBSD_CURL_CMAKE_OPTIONS)
	@touch '$@'

$(ZEDBSD_CURL_STAGED): $(ZEDBSD_CURL_CONFIGURED)
	@rm -rf '$(ZEDBSD_EXT_curl_STAGEDIR)'
	cmake --build '$(ZEDBSD_EXT_curl_BUILDDIR)' --parallel $(ZEDBSD_BUILD_JOBS)
	cmake --install '$(ZEDBSD_EXT_curl_BUILDDIR)' \
		--prefix '$(ZEDBSD_EXT_curl_STAGEDIR)/usr'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_CURL_SONAME) \
		--needed libssl.so --needed libcrypto.so --needed libz.so.1 \
		--needed libc.so '$(ZEDBSD_CURL_LIBRARY)'
	@# The program links what the library links as well, as curl's build does.
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role application \
		--needed $(ZEDBSD_CURL_SONAME) --needed libssl.so \
		--needed libcrypto.so --needed libz.so.1 --needed libc.so \
		'$(ZEDBSD_CURL_PROGRAM)'
	@touch '$@'

$(ZEDBSD_CURL_LIBRARY) $(ZEDBSD_CURL_PROGRAM) $(ZEDBSD_CURL_PC): \
	$(ZEDBSD_CURL_STAGED)
	@test -f '$@'

$(ZEDBSD_CURL_LICENSE): $(ZEDBSD_EXT_curl_SRCSTAMP)
	@test -f '$@'

.PHONY: curl
curl: $(ZEDBSD_CURL_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# Choosing curl chooses what it is built against and the CA bundle it reads.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,curl,curl and libcurl,*,n,package,,y,\
	packages/network,network/curl,security/openssl libs/zlib security/ca-certificates))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter curl,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_CURL_LIBRARY) $(ZEDBSD_CURL_PROGRAM) $(ZEDBSD_CURL_PC) \
	$(ZEDBSD_CURL_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter curl,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_CURL_SONAME)=$(ZEDBSD_CURL_LIBRARY) \
	--file /usr/bin/curl=$(ZEDBSD_CURL_PROGRAM) \
	$(foreach header,curl.h curlver.h easy.h header.h mprintf.h multi.h \
		options.h stdcheaders.h system.h typecheck-gcc.h urlapi.h websockets.h,\
		--file /usr/include/curl/$(header)=$(ZEDBSD_EXT_curl_STAGEDIR)/usr/include/curl/$(header)) \
	--file /usr/lib/pkgconfig/libcurl.pc=$(ZEDBSD_CURL_PC) \
	--file /usr/share/licenses/curl/COPYING=$(ZEDBSD_CURL_LICENSE))
ZEDBSD_PACKAGE_LINKS += $(if $(filter curl,$(ZEDBSD_USER_PROGRAMS)),\
	/usr/lib/libcurl.so=$(ZEDBSD_CURL_SONAME))
