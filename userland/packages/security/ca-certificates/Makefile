# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# ca-certificates (Mozilla's roots)
#
# The trusted roots are one PEM file that curl extracts from Mozilla's
# certdata.txt, keeping only the roots Mozilla trusts to identify TLS
# servers.  The file is used as published; nothing converts it.  The
# certificate data is MPL-2.0, whose text sits beside this Makefile.
#
# It is installed as /etc/ssl/cert.pem, which is the default CA file
# of the libssl-compat library, so a program that asks libssl-compat
# for its default paths finds it without being told.
#
# To update: take the newest dated cacert-YYYY-MM-DD.pem from
# https://curl.se/docs/caextract.html, record its size and the SHA-256
# curl publishes beside it, and check that every certificate still
# parses.
#

CA_CERTIFICATES_MAKEFILE := $(lastword $(MAKEFILE_LIST))
CA_CERTIFICATES_DIR      := $(patsubst %/,%,$(dir $(CA_CERTIFICATES_MAKEFILE)))

include $(CA_CERTIFICATES_DIR)/../../../base/package.mk
include $(CA_CERTIFICATES_DIR)/../../external.mk

# "Certificate data from Mozilla as of: Thu Aug 13 03:12:01 2026 GMT", 121 roots.
EXT_ca-certificates_VERSION := 2026-08-13
EXT_ca-certificates_ARCHIVE := cacert-2026-08-13.pem
EXT_ca-certificates_URL := https://curl.se/ca/cacert-2026-08-13.pem
EXT_ca-certificates_SIZE := 188900
EXT_ca-certificates_SHA256 := f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9

$(eval $(call EXTERNAL_FILE,ca-certificates))

CA_CERTIFICATES_LICENSE := $(CA_CERTIFICATES_DIR)/MPL-2.0.txt

.PHONY: ca-certificates
ca-certificates: $(EXT_ca-certificates_ARCHIVE_VERIFIED)
	@:

build: ca-certificates

$(eval \
	$(call \
		USERLAND_PACKAGE, \
		ca-certificates, \
		Trusted CA certificates, \
		*, \
		n, \
		package, \
		, \
		y, \
		packages/security, \
		security/ca-certificates, \
		, \
		0644, \
		custom \
	)\
)

PACKAGE_INPUTS += \
	$(if $(filter ca-certificates,$(USERLAND_PROGRAMS) $(USER_PROGRAMS) $(ZEDBSD_USER_PROGRAMS)),\
		$(EXT_ca-certificates_ARCHIVE_VERIFIED) \
		$(CA_CERTIFICATES_LICENSE))

PACKAGE_FILES += \
	$(if $(filter ca-certificates,$(USERLAND_PROGRAMS) $(USER_PROGRAMS) $(ZEDBSD_USER_PROGRAMS)),\
		--file /etc/ssl/cert.pem=$(EXT_ca-certificates_DISTFILE) \
		--file /usr/share/licenses/ca-certificates/MPL-2.0.txt=$(CA_CERTIFICATES_LICENSE) \
		--mode /etc/ssl/cert.pem=0644 \
		--mode /usr/share/licenses/ca-certificates/MPL-2.0.txt=0644)
