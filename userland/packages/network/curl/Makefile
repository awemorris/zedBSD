# zedBSD userland package: curl and libcurl (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# curl is under its own MIT-style licence and stays outside the source tree:
# only this Makefile and the recorded archive identity live here.
# Provenance is in plan/ws034/package-inventory.md.
CURL_MAKEFILE := $(lastword $(MAKEFILE_LIST))
CURL_DIR := $(patsubst %/,%,$(dir $(CURL_MAKEFILE)))
include $(CURL_DIR)/../../../base/package.mk
include $(CURL_DIR)/../../external.mk

# 8.22.0 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked with the
# maintainer's key (WS034 p001).
EXT_curl_VERSION := 8.22.0
EXT_curl_ROOT := curl-8.22.0
EXT_curl_ARCHIVE := curl-8.22.0.tar.xz
EXT_curl_URL := https://curl.se/download/curl-8.22.0.tar.xz
EXT_curl_SIZE := 2953092
EXT_curl_SHA256 := f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
EXT_curl_PATCH_LEVEL := zedbsd0
EXT_curl_PATCHES :=

$(eval $(call EXTERNAL_SOURCE,curl))

# ---------------------------------------------------------------- build

CURL_SONAME := libcurl.so.4
CURL_CONFIGURED := $(EXT_curl_BUILDDIR)/.zedbsd-configured
CURL_STAGED := $(EXT_curl_STAGEDIR)/.zedbsd-staged
CURL_LIBRARY := $(EXT_curl_STAGEDIR)/usr/lib/libcurl.so.4.8.0
CURL_PROGRAM := $(EXT_curl_STAGEDIR)/usr/bin/curl
CURL_PC := $(EXT_curl_STAGEDIR)/usr/lib/pkgconfig/libcurl.pc
CURL_LICENSE := $(EXT_curl_SRCDIR)/COPYING

# The staged OpenSSL and zlib are named by path: this file is read before
# theirs, so their variables would still be empty here.  They are not in the
# sysroot, where the toolchain file confines CMake's searches, so each
# header directory and library is given to CMake directly.
CURL_SSL_STAGE := $(EXTERNAL_WORKROOT)/openssl/stage/usr
CURL_Z_STAGE := $(EXTERNAL_WORKROOT)/zlib/stage/usr
CURL_DEPENDENCIES := \
	$(EXTERNAL_WORKROOT)/openssl/stage/.zedbsd-staged \
	$(EXTERNAL_WORKROOT)/zlib/stage/.zedbsd-staged

# OpenSSL for TLS and zlib for compressed transfers.  libpsl, libidn2,
# libssh2, nghttp2, brotli, zstd, LDAP and GSSAPI are not built here and are
# turned off rather than left to be looked for.  The CA bundle is the one the
# ca-certificates package installs (WS034 p019); cross-compiling, CMake
# cannot find it by itself.  The manual pages need perl-built tools and are
# not made.  Symbol versioning stays off: the loader does not read it.
CURL_CMAKE_OPTIONS := \
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
	-DOPENSSL_ROOT_DIR='$(CURL_SSL_STAGE)' \
	-DOPENSSL_INCLUDE_DIR='$(CURL_SSL_STAGE)/include' \
	-DOPENSSL_SSL_LIBRARY='$(CURL_SSL_STAGE)/lib/libssl.so' \
	-DOPENSSL_CRYPTO_LIBRARY='$(CURL_SSL_STAGE)/lib/libcrypto.so' \
	-DCURL_ZLIB=ON \
	-DZLIB_INCLUDE_DIR='$(CURL_Z_STAGE)/include' \
	-DZLIB_LIBRARY='$(CURL_Z_STAGE)/lib/libz.so' \
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
	-DCMAKE_SHARED_LINKER_FLAGS='-Wl,-rpath-link,$(CURL_SSL_STAGE)/lib -Wl,-rpath-link,$(CURL_Z_STAGE)/lib' \
	-DCMAKE_EXE_LINKER_FLAGS='-Wl,-rpath-link,$(CURL_SSL_STAGE)/lib -Wl,-rpath-link,$(CURL_Z_STAGE)/lib'

# The options are read when the tree is configured, so this file is an input.
$(CURL_CONFIGURED): $(EXT_curl_SRCSTAMP) \
	$(EXTERNAL_CROSS_STAMP) $(CURL_DEPENDENCIES) \
	$(CURL_MAKEFILE)
	@rm -rf '$(EXT_curl_BUILDDIR)'
	@mkdir -p '$(EXT_curl_BUILDDIR)'
	cmake -S '$(EXT_curl_SRCDIR)' -B '$(EXT_curl_BUILDDIR)' \
		-G Ninja \
		--toolchain '$(EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(CURL_CMAKE_OPTIONS)
	@touch '$@'

$(CURL_STAGED): $(CURL_CONFIGURED)
	@rm -rf '$(EXT_curl_STAGEDIR)'
	cmake --build '$(EXT_curl_BUILDDIR)' --parallel $(BUILD_JOBS)
	cmake --install '$(EXT_curl_BUILDDIR)' \
		--prefix '$(EXT_curl_STAGEDIR)/usr'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(CURL_SONAME) \
		--needed libssl.so --needed libcrypto.so --needed libz.so.1 \
		--needed libc.so '$(CURL_LIBRARY)'
	@# The program links what the library links as well, as curl's build does.
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role application \
		--needed $(CURL_SONAME) --needed libssl.so \
		--needed libcrypto.so --needed libz.so.1 --needed libc.so \
		'$(CURL_PROGRAM)'
	@touch '$@'

$(CURL_LIBRARY) $(CURL_PROGRAM) $(CURL_PC): \
	$(CURL_STAGED)
	@test -f '$@'

$(CURL_LICENSE): $(EXT_curl_SRCSTAMP)
	@test -f '$@'

.PHONY: curl
curl: $(CURL_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# Choosing curl chooses what it is built against and the CA bundle it reads.
$(eval $(call USERLAND_PACKAGE,curl,curl and libcurl,*,n,package,,y,\
	packages/network,network/curl,security/openssl libs/zlib security/ca-certificates))

PACKAGE_INPUTS += $(if $(filter curl,$(USER_PROGRAMS)),\
	$(CURL_LIBRARY) $(CURL_PROGRAM) $(CURL_PC) \
	$(CURL_LICENSE))
PACKAGE_FILES += $(if $(filter curl,$(USER_PROGRAMS)),\
	--file /usr/lib/$(CURL_SONAME)=$(CURL_LIBRARY) \
	--file /usr/bin/curl=$(CURL_PROGRAM) \
	$(foreach header,curl.h curlver.h easy.h header.h mprintf.h multi.h \
		options.h stdcheaders.h system.h typecheck-gcc.h urlapi.h websockets.h,\
		--file /usr/include/curl/$(header)=$(EXT_curl_STAGEDIR)/usr/include/curl/$(header)) \
	--file /usr/lib/pkgconfig/libcurl.pc=$(CURL_PC) \
	--file /usr/share/licenses/curl/COPYING=$(CURL_LICENSE))
PACKAGE_LINKS += $(if $(filter curl,$(USER_PROGRAMS)),\
	/usr/lib/libcurl.so=$(CURL_SONAME))
