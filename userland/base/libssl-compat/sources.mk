# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The source list of libssl-compat, read by its Makefile and by the builds of the other systems: the Windows build
# (apps/browser-win32/Makefile) takes LIBSSL_COMPAT_SOURCES into one static library, the zedBSD build into a shared
# one.  No file depends on the operating system (the socket and the clock are libcrypto-compat's cryc-os).
#

LIBSSL_COMPAT_SOURCES := \
	userland/base/libssl-compat/api-ssl.c \
	userland/base/libssl-compat/sslc-buf.c \
	userland/base/libssl-compat/sslc-conn.c \
	userland/base/libssl-compat/sslc-hs.c \
	userland/base/libssl-compat/sslc-hs12.c \
	userland/base/libssl-compat/sslc-hs13.c \
	userland/base/libssl-compat/sslc-keysched.c \
	userland/base/libssl-compat/sslc-msg.c \
	userland/base/libssl-compat/sslc-record.c \
	userland/base/libssl-compat/sslc-tls.c
