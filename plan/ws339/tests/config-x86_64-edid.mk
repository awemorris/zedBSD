# The x86_64 image of the loader's monitor-size check (p001-qemu.sh): the base userland of
# plan/tools/gnu-utils/config-amd64-base.mk (no external packages and no firmware downloads), booted by the UEFI
# loader with the platform's standard zedbsd-uefi.cfg (no video=, no logo).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/gnu-utils/config-amd64-base.mk
