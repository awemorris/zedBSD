# ws190-p002: the AAT image (plan/tools/aat/config-amd64-aat.mk, whose aat-input has the touch screen) with kuidemo
# (userland/tests/kuidemo: the fields and the text area the fingers' selection is tried on).  A test image only.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/aat/config-amd64-aat.mk
ZEDBSD_USER_PROGRAMS += kuidemo
