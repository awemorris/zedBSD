# The x86_64 guest image of the render thread's waits (qemu-render-wait.sh) and of the WS309 guest tests run on it
# (plan/ws309/tests/resize-guest.sh, rotate-guest.sh, scanout-guest.sh, task-guest.sh, grave-guest.sh): the event-loop
# wake test's image (config-amd64-mainwake.mk: the compositor with its test-only capture, keiland-shot,
# keiland-settings, the SSH harness) with the clients those tests draw with (wlshm, wltest, mview), nohup (with which
# resize-guest.sh and rotate-guest.sh start the compositor) and the wallpaper they show.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(abspath $(dir $(lastword $(MAKEFILE_LIST))))/config-amd64-mainwake.mk

override CONFIG_USERLAND_PROGRAMS += wlshm wltest mview nohup

WS353_HARNESS_FILES += \
	--file /usr/share/keiland/wallpaper.png=$(WS353_TREE)/userland/desktop/wallpapers/Birch-Lake.png

# The rootfs is made again when this file changes too.
X86_64_ARCH_INPUTS += $(WS353_TREE)/plan/ws353/tests/config-amd64-render.mk
