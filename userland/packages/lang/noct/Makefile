# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,noct,Noct,i386 pc98 amd64,y,package,,y,packages/lang,lang/noct,))

NOCT_REPOSITORY ?= https://github.com/awemorris/NoctLang.git
NOCT_GIT_REF ?= main
NOCT_SOURCE_DIR := build/sources/noct
NOCT_SOURCE_STAMP := $(NOCT_SOURCE_DIR)/.zedbsd-source
NOCT_PACKAGE_HOLORIS := $(NOCT_SOURCE_DIR)/apps/holoris/holoris.noct
ZEDBSD_PACKAGE_INPUTS += $(if $(filter noct,$(ZEDBSD_USER_PROGRAMS)),\
	$(BUILD)/bin/noct $(NOCT_PACKAGE_HOLORIS))
ZEDBSD_PACKAGE_FILES += $(if $(filter noct,$(ZEDBSD_USER_PROGRAMS)),\
	--file $(ZEDBSD_PACKAGE_BINDIR)/noct=$(BUILD)/bin/noct \
	--file $(ZEDBSD_PACKAGE_BINDIR)/holoris.nct=$(NOCT_PACKAGE_HOLORIS))

$(NOCT_SOURCE_STAMP):
	@mkdir -p $(dir $(NOCT_SOURCE_DIR))
	git clone --depth 1 --branch $(NOCT_GIT_REF) \
		$(NOCT_REPOSITORY) $(NOCT_SOURCE_DIR)
	@git -C $(NOCT_SOURCE_DIR) rev-parse HEAD > $@

# Package resources that live in Noct resolve through the source stamp.
$(NOCT_PACKAGE_HOLORIS): | $(NOCT_SOURCE_STAMP)
	@test -e $@ || { echo "Noct source is missing after clone: $@" >&2; exit 1; }

.PHONY: noct-source
noct-source: $(NOCT_SOURCE_STAMP)
