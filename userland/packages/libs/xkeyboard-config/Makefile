# zedBSD userland package: xkeyboard-config (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# xkeyboard-config is data under MIT-style licences (its COPYING collects
# them) and stays outside the source tree: only this Makefile, the recorded
# archive identity and the list of installed files live here.  libxkbcommon
# builds keymaps from it by name; GTK makes its default keymap that way when
# it starts (WS115, port-contract §7).  Provenance is in
# plan/ws034/package-inventory.md and plan/ws115/port-contract.md.
XKEYBOARD_CONFIG_MAKEFILE := $(lastword $(MAKEFILE_LIST))
XKEYBOARD_CONFIG_DIR := $(patsubst %/,%,$(dir $(XKEYBOARD_CONFIG_MAKEFILE)))
include $(XKEYBOARD_CONFIG_DIR)/../../../base/package.mk
include $(XKEYBOARD_CONFIG_DIR)/../../external.mk

# 2.48 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked
# (WS034 p001).
EXT_xkeyboard-config_VERSION := 2.48
EXT_xkeyboard-config_ROOT := xkeyboard-config-2.48
EXT_xkeyboard-config_ARCHIVE := xkeyboard-config-2.48.tar.xz
EXT_xkeyboard-config_URL := https://www.x.org/releases/individual/data/xkeyboard-config/xkeyboard-config-2.48.tar.xz
EXT_xkeyboard-config_SIZE := 953316
EXT_xkeyboard-config_SHA256 := b77041324f0109f77161ee43743fe04baa485866af8460d31e476ad3f7648fd5
EXT_xkeyboard-config_PATCH_LEVEL := zedbsd0
EXT_xkeyboard-config_PATCHES :=

$(eval $(call EXTERNAL_SOURCE,xkeyboard-config))

# ---------------------------------------------------------------- build

# Meson, through the common rule; nothing is compiled.  The compatibility
# rules stay; translations and the X.Org rule-name links are off.
EXT_xkeyboard-config_MAKEFILE := $(XKEYBOARD_CONFIG_MAKEFILE)
EXT_xkeyboard-config_DEPENDS :=
EXT_xkeyboard-config_HOST_TOOLS :=
EXT_xkeyboard-config_MESON_OPTIONS := \
	-Dcompat-rules=true -Dxorg-rules-symlinks=false -Dnls=false \
	-Dnon-latin-layouts-list=false

$(eval $(call EXTERNAL_MESON,xkeyboard-config))

XKEYBOARD_CONFIG_STAGED := $(EXT_xkeyboard-config_STAGEDIR)/.zedbsd-staged
XKEYBOARD_CONFIG_CHECKED := $(EXT_xkeyboard-config_STAGEDIR)/.zedbsd-checked
XKEYBOARD_CONFIG_ROOT := $(EXT_xkeyboard-config_STAGEDIR)/usr/share/xkeyboard-config-2
XKEYBOARD_CONFIG_LICENSE := $(EXT_xkeyboard-config_SRCDIR)/COPYING

# The data files the image carries, relative to the data root: the rules,
# keycodes, types, compat and symbols libxkbcommon reads.  The geometry
# files are left out because libxkbcommon never reads them.  The image is
# built from a file list known when make starts, before the stage exists, so
# the list is kept here, and the check below fails when the stage and the
# list differ (a new release regenerates it with the find command there).
XKEYBOARD_CONFIG_LIST := $(XKEYBOARD_CONFIG_DIR)/files/data.list
XKEYBOARD_CONFIG_DATA := $(file <$(XKEYBOARD_CONFIG_LIST))

# Checks that the stage holds exactly the listed data files.
$(XKEYBOARD_CONFIG_CHECKED): $(XKEYBOARD_CONFIG_STAGED) \
	$(XKEYBOARD_CONFIG_LIST)
	@cd '$(XKEYBOARD_CONFIG_ROOT)' && \
		LC_ALL=C find compat keycodes rules symbols types -type f | LC_ALL=C sort | \
		cmp -s - '$(abspath $(XKEYBOARD_CONFIG_LIST))' || { \
			echo "xkeyboard-config: the stage differs from files/data.list" >&2; \
			exit 1; \
		}
	@touch '$@'

$(XKEYBOARD_CONFIG_LICENSE): $(EXT_xkeyboard-config_SRCSTAMP)
	@test -f '$@'

.PHONY: xkeyboard-config
xkeyboard-config: $(XKEYBOARD_CONFIG_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The data under /usr/share/xkeyboard-config-2, where libxkbcommon is built
# to look, and the traditional /usr/share/X11/xkb pointing at it.
$(eval $(call USERLAND_PACKAGE,xkeyboard-config,Keyboard layout data,*,n,package,,y,\
	packages/libs,libs/xkeyboard-config,))

PACKAGE_INPUTS += $(if $(filter xkeyboard-config,$(USER_PROGRAMS)),\
	$(XKEYBOARD_CONFIG_CHECKED) $(XKEYBOARD_CONFIG_LICENSE))
PACKAGE_FILES += $(if $(filter xkeyboard-config,$(USER_PROGRAMS)),\
	$(foreach data,$(XKEYBOARD_CONFIG_DATA),\
		--file /usr/share/xkeyboard-config-2/$(data)=$(XKEYBOARD_CONFIG_ROOT)/$(data)) \
	--file /usr/share/licenses/xkeyboard-config/COPYING=$(XKEYBOARD_CONFIG_LICENSE))
PACKAGE_LINKS += $(if $(filter xkeyboard-config,$(USER_PROGRAMS)),\
	/usr/share/X11/xkb=../xkeyboard-config-2)
