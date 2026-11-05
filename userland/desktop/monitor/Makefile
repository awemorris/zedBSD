include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The System Monitor (plan/ws134/design.md): the machine's CPU, GPU, memory, network and disks as plates in layers, the
# system's state at the centre, drawn with Vulkan in a Keiland window; the values from a simulation, a recording, and
# later the system through libkeiland.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
KEILAND_MONITOR_SOURCES := userland/desktop/monitor/main.c \
	userland/desktop/monitor/source.c userland/desktop/monitor/history.c \
	userland/desktop/monitor/rules.c userland/desktop/monitor/format.c \
	userland/desktop/monitor/atlas.c userland/desktop/monitor/draw.c userland/desktop/monitor/scene.c userland/desktop/monitor/space.c userland/desktop/monitor/interact.c \
	userland/desktop/monitor/system-source.c \
	userland/desktop/monitor/render.c
$(eval $(call USERLAND_PACKAGE,monitor,monitor - System monitor,x86_64,y,basic,$(KEILAND_MONITOR_SOURCES),y,desktop,desktop/monitor,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype,0755,program,,,bin))
