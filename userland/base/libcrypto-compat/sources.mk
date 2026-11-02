# The source lists of libcrypto-compat, read by its Makefile and by the builds of the other systems: the Windows
# build (apps/browser-win32/Makefile) takes LIBCRYPTO_COMPAT_SOURCES and LIBCRYPTO_COMPAT_WIN32_SOURCES into one
# static library, the zedBSD build takes LIBCRYPTO_COMPAT_SOURCES and LIBCRYPTO_COMPAT_POSIX_SOURCES into a shared one.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# Every file that does not depend on the operating system.
LIBCRYPTO_COMPAT_SOURCES := \
	userland/base/libcrypto-compat/api-err.c \
	userland/base/libcrypto-compat/api-evp.c \
	userland/base/libcrypto-compat/api-kdf.c \
	userland/base/libcrypto-compat/api-param.c \
	userland/base/libcrypto-compat/api-pkey.c \
	userland/base/libcrypto-compat/api-x509.c \
	userland/base/libcrypto-compat/cryc-aes.c \
	userland/base/libcrypto-compat/cryc-bn.c \
	userland/base/libcrypto-compat/cryc-chacha.c \
	userland/base/libcrypto-compat/cryc-chain.c \
	userland/base/libcrypto-compat/cryc-der.c \
	userland/base/libcrypto-compat/cryc-ecc.c \
	userland/base/libcrypto-compat/cryc-ecp.c \
	userland/base/libcrypto-compat/cryc-ed25519.c \
	userland/base/libcrypto-compat/cryc-err.c \
	userland/base/libcrypto-compat/cryc-f25519.c \
	userland/base/libcrypto-compat/cryc-gcm.c \
	userland/base/libcrypto-compat/cryc-hash.c \
	userland/base/libcrypto-compat/cryc-mlkem-poly.c \
	userland/base/libcrypto-compat/cryc-mlkem.c \
	userland/base/libcrypto-compat/cryc-name.c \
	userland/base/libcrypto-compat/cryc-pem.c \
	userland/base/libcrypto-compat/cryc-rsa.c \
	userland/base/libcrypto-compat/cryc-sha2.c \
	userland/base/libcrypto-compat/cryc-sha3.c \
	userland/base/libcrypto-compat/cryc-sigver.c \
	userland/base/libcrypto-compat/cryc-util.c \
	userland/base/libcrypto-compat/cryc-x25519.c \
	userland/base/libcrypto-compat/cryc-x509.c

# The files that depend on the operating system, one per system.
LIBCRYPTO_COMPAT_POSIX_SOURCES := userland/base/libcrypto-compat/cryc-os-posix.c
LIBCRYPTO_COMPAT_WIN32_SOURCES := userland/base/libcrypto-compat/cryc-os-win32.c
