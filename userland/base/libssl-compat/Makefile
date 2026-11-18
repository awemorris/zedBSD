# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libssl-compat: zedBSD's own TLS client with the part of OpenSSL's libssl interface that the browser uses; no
# OpenSSL code.  It is built on libcrypto-compat.  libbrowser and mailer link it: a build that makes either
# makes it first, and the image takes every shared library the build made.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

LIBSSL_COMPAT_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libssl-compat.so=$(BUILD)/dynamic/libssl-compat.so)
$(eval $(call USERLAND_PACKAGE,libssl-compat,libssl-compat,*,n,library,$(LIBSSL_COMPAT_SOURCES),y,base,base/libssl-compat,base/libcrypto-compat,,shared-library,$(LIBSSL_COMPAT_IMAGE_DATA),,lib))
