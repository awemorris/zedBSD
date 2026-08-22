# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,noct,Noct,i386 pc98 amd64,y,package,,y,packages/lang,lang/noct,))

NOCT_ROOT ?= userland/noct
NOCT_SOURCE_SENTINEL ?= $(NOCT_ROOT)/CMakeLists.txt
NOCT_SOURCE_DIR := $(NOCT_ROOT)
NOCT_PACKAGE_HOLORIS := $(NOCT_SOURCE_DIR)/apps/holoris/holoris.noct
ZEDBSD_PACKAGE_INPUTS += $(if $(filter noct,$(ZEDBSD_USER_PROGRAMS)),\
	$(BUILD)/bin/noct $(NOCT_PACKAGE_HOLORIS))
ZEDBSD_PACKAGE_FILES += $(if $(filter noct,$(ZEDBSD_USER_PROGRAMS)),\
	--file $(ZEDBSD_PACKAGE_BINDIR)/noct=$(BUILD)/bin/noct \
	--file $(ZEDBSD_PACKAGE_BINDIR)/holoris.nct=$(NOCT_PACKAGE_HOLORIS))

$(NOCT_SOURCE_SENTINEL):
	@echo "Noct submodule is not initialized." >&2
	@echo "Run: git submodule update --init userland/noct" >&2
	@false

# Package resources live in the pinned Noct submodule revision.
$(NOCT_PACKAGE_HOLORIS): | $(NOCT_SOURCE_SENTINEL)
	@test -e $@ || { echo "Noct submodule source is missing: $@" >&2; exit 1; }

.PHONY: noct-source
noct-source: $(NOCT_SOURCE_SENTINEL)
