include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# fidoctl (ws161-p004): a security key's information, PIN, credentials and assertions over libpasskey (the raw HID
# nodes /dev/input/hidrawN, and an NFC reader's smart card slots /dev/smartcardN, ws161-p005).  libpasskey's sources
# are built into it; the cryptography is libcrypto-compat (libpasskey's crypto-openssl.c, written for OpenSSL's
# interface), so it is linked by its own rule in the platform's makefile.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
FIDOCTL_SOURCES := userland/base/fidoctl/main.c userland/base/libpasskey/cbor.c userland/base/libpasskey/crypto-openssl.c \
	userland/base/libpasskey/ctap2.c userland/base/libpasskey/descriptor.c userland/base/libpasskey/hid.c \
	userland/base/libpasskey/os-posix.c userland/base/libpasskey/os-zedbsd.c userland/base/libpasskey/pin.c \
	userland/base/libpasskey/transport-nfc.c userland/base/libpasskey/verify.c
$(eval $(call USERLAND_PACKAGE,fidoctl,fidoctl,x86_64,n,basic,$(FIDOCTL_SOURCES),y,base,base/fidoctl,base/libcrypto-compat,0755,program,,,bin))
