# zedBSD userland package: ca-certificates (Mozilla's roots, as curl ships them).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The trusted roots are one PEM file that curl extracts from Mozilla's
# certdata.txt, keeping only the roots Mozilla trusts to identify TLS servers.
# The file is used as published; nothing converts it.  The certificate data is
# MPL-2.0, whose text sits beside this Makefile.  Provenance is in
# plan/ws034/package-inventory.md and plan/ws034/phase019/phase.md.
#
# It is installed as /etc/ssl/cert.pem, which is the default CA file of the
# OpenSSL package (configured with --openssldir=/etc/ssl), so a program that
# asks OpenSSL for its default paths finds it without being told.
#
# To update: take the newest dated cacert-YYYY-MM-DD.pem from
# https://curl.se/docs/caextract.html, record its size and the SHA-256 curl
# publishes beside it, and check that every certificate still parses.
ZEDBSD_CA_CERTIFICATES_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_CA_CERTIFICATES_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_CA_CERTIFICATES_MAKEFILE)))
include $(ZEDBSD_CA_CERTIFICATES_DIR)/../../../base/package.mk
include $(ZEDBSD_CA_CERTIFICATES_DIR)/../../external.mk

# "Certificate data from Mozilla as of: Thu Aug 13 03:12:01 2026 GMT", 121 roots.
ZEDBSD_EXT_ca-certificates_VERSION := 2026-08-13
ZEDBSD_EXT_ca-certificates_ARCHIVE := cacert-2026-08-13.pem
ZEDBSD_EXT_ca-certificates_URL := https://curl.se/ca/cacert-2026-08-13.pem
ZEDBSD_EXT_ca-certificates_SIZE := 188900
ZEDBSD_EXT_ca-certificates_SHA256 := f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9

$(eval $(call ZEDBSD_EXTERNAL_FILE,ca-certificates))

ZEDBSD_CA_CERTIFICATES_LICENSE := $(ZEDBSD_CA_CERTIFICATES_DIR)/MPL-2.0.txt

.PHONY: ca-certificates
ca-certificates: $(ZEDBSD_EXT_ca-certificates_ARCHIVE_VERIFIED)
	@:

# ---------------------------------------------------------------- packaging

$(eval $(call ZEDBSD_USERLAND_PACKAGE,ca-certificates,Trusted CA certificates,*,n,package,,y,\
	packages/security,security/ca-certificates,))

# The verified record is the input, so the image waits for the check rather
# than for the bare download.  The download keeps the fetcher's private mode,
# so the modes are set: every program that makes a TLS connection reads it.
ZEDBSD_PACKAGE_INPUTS += $(if $(filter ca-certificates,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_EXT_ca-certificates_ARCHIVE_VERIFIED) $(ZEDBSD_CA_CERTIFICATES_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter ca-certificates,$(ZEDBSD_USER_PROGRAMS)),\
	--file /etc/ssl/cert.pem=$(ZEDBSD_EXT_ca-certificates_DISTFILE) \
	--file /usr/share/licenses/ca-certificates/MPL-2.0.txt=$(ZEDBSD_CA_CERTIFICATES_LICENSE) \
	--mode /etc/ssl/cert.pem=0644 \
	--mode /usr/share/licenses/ca-certificates/MPL-2.0.txt=0644)
