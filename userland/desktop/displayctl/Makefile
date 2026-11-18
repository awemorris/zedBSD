include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# displayctl: the displays of the desktop and their modes, listed and changed through the compositor
# (libkeiland's kl_system_displays_*).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
DISPLAYCTL_SOURCES := userland/desktop/displayctl/main.c
$(eval $(call USERLAND_PACKAGE,displayctl,displayctl - Display modes,x86_64,y,basic,$(DISPLAYCTL_SOURCES),y,desktop,desktop/displayctl,desktop/libwayland desktop/libkeiland,0755,program,,,bin))
