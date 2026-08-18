# Architecture-specific standalone root filesystem image support.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ARCH_IMAGE_DIR := build/arch-images
ARCH_IMAGE_TOOLS := scripts/make-arch-overlay-image.py \
	scripts/check-arch-overlay-image.py
ARCH_UFS_IMAGE_TOOLS := scripts/make-arch-overlay-ufs.py \
	scripts/check-arch-overlay-ufs.py scripts/check-ufs1-image.py \
	scripts/check_ufs1_import.py scripts/ufs1_format.py

# $(1): output, $(2): profile, $(3): prerequisites, $(4): repeated --file args.
define ZEDBSD_ARCH_IMAGE_RULE
$(1): $(3) $(ARCH_IMAGE_TOOLS)
	@mkdir -p $$(dir $$@)
	$$(PYTHON) scripts/make-arch-overlay-image.py --force \
		--profile $(2) --output $$@ $(4)

$(1)-check: $(1)
	$$(PYTHON) scripts/check-arch-overlay-image.py --profile $(2) \
		--image $$< $(4)
endef

# $(1): output, $(2): profile, $(3): prerequisites, $(4): repeated --file args.
define ZEDBSD_ARCH_UFS_IMAGE_RULE
$(1): $(3) $(ARCH_UFS_IMAGE_TOOLS)
	@mkdir -p $$(dir $$@)
	$$(PYTHON) scripts/make-arch-overlay-ufs.py --force \
		--profile $(2) --output $$@ $(4)

$(1)-check: $(1)
	$$(PYTHON) scripts/check-arch-overlay-ufs.py --profile $(2) \
		--image $$< $(4)
endef

