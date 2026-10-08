# ws189-p002: the AAT image (plan/tools/aat/config-amd64-aat.mk) with data-probe (userland/tests/data-probe), the
# Wayland client that reads a drag's data at its enter and at its drop (desktop.dnd.early-receive).  A test image only.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include plan/tools/aat/config-amd64-aat.mk
ZEDBSD_USER_PROGRAMS += data-probe
