# The Venus guest image of ws243-p006's Video Player check: the menu test image (Keiland on Venus) with Video Player,
# libmedia and libavcodec (the guest's GPU has no video decoder, so the H.264 clip is decoded by the CPU and the
# player's own presenter draws the pictures the CPU scaled).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/titlebar/config-amd64-menu.mk
override CONFIG_USERLAND_PROGRAMS += libmedia libavcodec videoplayer
