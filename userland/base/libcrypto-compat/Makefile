include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libcrypto-compat: zedBSD's own libcrypto interface (the part of OpenSSL's libcrypto that the TLS library, the
# browser and the SSH programs use); no OpenSSL code.  Off by default: a program that links it selects it.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

LIBCRYPTO_COMPAT_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libcrypto-compat.so=$(BUILD)/dynamic/libcrypto-compat.so)
$(eval $(call USERLAND_PACKAGE,libcrypto-compat,libcrypto-compat,*,n,library,$(LIBCRYPTO_COMPAT_SOURCES) $(LIBCRYPTO_COMPAT_POSIX_SOURCES),y,base,base/libcrypto-compat,,,shared-library,$(LIBCRYPTO_COMPAT_IMAGE_DATA),,lib))
