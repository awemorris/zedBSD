include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# /usr/libexec/passkey-fido2 (ws172-p003; docs/architecture/security.md, "Login authentication"): the security key
# style of /sbin/passkey, which starts it.  Root only, mode 0500, not set-user-ID.  libpasskey's sources are built
# into it; the cryptography is OpenSSL's libcrypto until the release (the Guardrail's exception), so it is linked by
# its own rule in the platform's makefile.  Its device helper runs as _passkey in /var/empty.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PASSKEY_FIDO2_SOURCES := userland/base/passkey-fido2/main.c userland/base/passkey-fido2/device.c \
	userland/base/passkey-fido2/helper.c userland/base/passkey-fido2/wire.c userland/base/passkey/record.c \
	userland/base/passkey/request.c userland/base/common/account.c userland/base/login/verify.c \
	userland/base/libpasskey/cbor.c userland/base/libpasskey/crypto-openssl.c userland/base/libpasskey/ctap2.c \
	userland/base/libpasskey/descriptor.c userland/base/libpasskey/hid.c userland/base/libpasskey/os-posix.c \
	userland/base/libpasskey/os-zedbsd.c userland/base/libpasskey/pin.c userland/base/libpasskey/verify.c \
	userland/base/libpasskey/transport-nfc.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,passkey-fido2,passkey-fido2,amd64,n,basic,$(PASSKEY_FIDO2_SOURCES),y,base,base/passkey-fido2,base/passkey security/openssl,0500,program,,,usr/libexec))
