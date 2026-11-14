# ws269-p010: amd64 Venus test image with vkcompute, vkimage and vkstage
# (gpu-selftest's clear, copy, draw, sample, depth and present stages through Venus).
#   make CONFIGMK=plan/tools/powervr/p010/config-amd64-vkstage.mk BUILD=build/<dir> disk
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
include $(TEST_TREE)/plan/tools/guest/config-amd64-ssh.mk
override CONFIG_USERLAND_PROGRAMS += libwayland libvulkan vkcompute vkimage vkstage
