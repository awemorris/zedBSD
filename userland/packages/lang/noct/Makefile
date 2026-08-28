# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Temporarily unavailable as a zedBSD target package while the Principal
# Engineer repairs the canonical Noct source.  The host build-script toolchain
# is separate and remains enabled by the top-level Makefile.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,noct,Noct,i386 pc98 amd64,n,package,,n,packages/lang,lang/noct,))

NOCT_ROOT ?= userland/noct/NoctLang
NOCT_SOURCE_SENTINEL ?= $(NOCT_ROOT)/CMakeLists.txt
NOCT_SOURCE_DIR := $(NOCT_ROOT)
NOCT_PACKAGE_HOLORIS := $(NOCT_SOURCE_DIR)/apps/holoris/holoris.noct
NOCT_ZEDBSD_BUILD_DIR := $(NOCT_SOURCE_DIR)/build-zedbsd
NOCT_ZEDBSD_ARTIFACT := $(NOCT_ZEDBSD_BUILD_DIR)/noct
ZEDBSD_PACKAGE_INPUTS += $(if $(filter noct,$(ZEDBSD_USER_PROGRAMS)),\
	$(BUILD)/bin/noct $(NOCT_PACKAGE_HOLORIS))
ZEDBSD_PACKAGE_FILES += $(if $(filter noct,$(ZEDBSD_USER_PROGRAMS)),\
	--file $(ZEDBSD_PACKAGE_BINDIR)/noct=$(BUILD)/bin/noct \
	--file $(ZEDBSD_PACKAGE_BINDIR)/holoris.nct=$(NOCT_PACKAGE_HOLORIS))

$(NOCT_SOURCE_SENTINEL):
	$(MAKE) -C userland/noct checkout
	@test -e $@ || { echo "NoctLang source is missing: $@" >&2; exit 1; }

# Package resources live in the pinned official NoctLang checkout.
$(NOCT_PACKAGE_HOLORIS): | $(NOCT_SOURCE_SENTINEL)
	@test -e $@ || { echo "NoctLang source is missing: $@" >&2; exit 1; }

.PHONY: noct-source
noct-source: $(NOCT_SOURCE_SENTINEL)

ifeq ($(ZEDBSD_ARCHITECTURE),amd64)
.PHONY: noct-zedbsd
noct-zedbsd: | $(NOCT_SOURCE_SENTINEL)
	cd $(NOCT_SOURCE_DIR) && \
		ZEDBSD_SOURCE_DIR=$(abspath .) cmake --preset zedbsd
	cd $(NOCT_SOURCE_DIR) && \
		ZEDBSD_SOURCE_DIR=$(abspath .) \
		cmake --build --preset zedbsd --parallel 16
	@test -x $(NOCT_ZEDBSD_ARTIFACT)

$(BUILD)/bin/noct: noct-zedbsd
	@mkdir -p $(dir $@)
	cp -f $(NOCT_ZEDBSD_ARTIFACT) $@
	@cmp -s $(NOCT_ZEDBSD_ARTIFACT) $@ || { \
		echo "Installed Noct differs from the canonical CMake artifact" >&2; \
		exit 1; \
	}
endif
