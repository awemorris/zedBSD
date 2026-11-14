# ws189-p002: the AAT image (plan/tools/aat/config-amd64-aat.mk) with data-probe (userland/tests/data-probe), the
# Wayland client that reads a drag's data at its enter and at its drop (desktop.dnd.early-receive).  A test image only.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/aat/config-amd64-aat.mk
override CONFIG_USERLAND_PROGRAMS += data-probe
