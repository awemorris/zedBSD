# zedBSD unified IBM PC/AT + NEC PC-98 disk-image platform.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# This is an image-composition platform, not a kernel architecture.  The
# shared pc-unified rules recursively build the real pcat and pc98 kernels.

UNIFIED_PLATFORM_IMAGE := $(BUILD)/hdd-image.img
UNIFIED_CANONICAL_IMAGE := build/pc-unified/hdd-image.img

all: hdd-image

$(UNIFIED_PLATFORM_IMAGE): $(UNIFIED_CANONICAL_IMAGE)
	@mkdir -p $(dir $@)
	cp --reflink=auto -f $< $@
	$(PYTHON) $(SCRIPTS_DIR)/check-pc-unified-hdd-image.py \
		--pc98-kernel build/pc-unified/vmunix.98 \
		--pcat-kernel build/pc-unified/vmunix.at $@

hdd-image: $(UNIFIED_PLATFORM_IMAGE)
	@echo "Unified PC/AT + PC-98 HDD image: $(UNIFIED_PLATFORM_IMAGE)"

.PHONY: all hdd-image

