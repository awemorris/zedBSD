# The amd64 CI selection without OpenSSL and the packages that need it (curl, OpenSSH, Python), so that a build of
# passkey-fido2 and fidoctl shows they need nothing of OpenSSL: their libcrypto is libcrypto-compat (pulled in as
# their package requirement).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include config/ci/config-amd64.mk
ZEDBSD_USER_PROGRAMS := $(filter-out openssl curl openssh python3,$(ZEDBSD_USER_PROGRAMS))
