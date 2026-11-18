# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libcrypto-compat: zedBSD's own libcrypto interface (the part of OpenSSL's libcrypto that the TLS library, the
# browser, Mail and the SSH programs use); no OpenSSL code.  libssl-compat, libbrowser, mailer, ssh, fidoctl and
# passkey-fido2 link it: a build that makes one of them makes it first, and the image takes every shared library
# the build made.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

LIBCRYPTO_COMPAT_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libcrypto-compat.so=$(BUILD)/dynamic/libcrypto-compat.so)
$(eval $(call USERLAND_PACKAGE,libcrypto-compat,libcrypto-compat,*,n,library,$(LIBCRYPTO_COMPAT_SOURCES) $(LIBCRYPTO_COMPAT_POSIX_SOURCES),y,base,base/libcrypto-compat,,,shared-library,$(LIBCRYPTO_COMPAT_IMAGE_DATA),,lib))
