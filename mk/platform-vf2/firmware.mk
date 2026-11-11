# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
#

#
# VisionFive 2 device trees
#
# The vendor U-Boot's control device tree lacks the mainline compatibles the
# kernel looks for, so the card carries the mainline Linux device trees of the
# board's v1.2a and v1.3b revisions (GPL-2.0 OR MIT, dtb.LICENSE goes beside
# them).  They were compiled from Linux v6.12 (adc218676eef) with DTC 1.7.2
# and are kept in the zedBSD firmware repository's visionfive2-dtb/; a build
# fetches each into a cache and checks its size and SHA-256.
#

include userland/firmware/zedbsd-firmware.mk

# NAME|SIZE|SHA-256 of each device tree.
override VF2_DTB_FILES := \
	jh7110-starfive-visionfive-2-v1.2a.dtb|31655|e76b80a6ea4e3f1642865b0d7205ae20aa0195c1e218936150ba594e11fc39d0 \
	jh7110-starfive-visionfive-2-v1.3b.dtb|32040|1826e14d92c1ca336919f810f8f6c98102ed0065a9b9581087cf933b27f347ac

override VF2_DTB_URL := $(ZEDBSD_FIRMWARE_REPOSITORY_URL)/visionfive2-dtb

VF2_DTB_CACHE_ROOT ?= build/sources/firmware/visionfive2-dtb

override VF2_DTB_DIR := $(VF2_DTB_CACHE_ROOT)/$(ZEDBSD_FIRMWARE_REPOSITORY_REVISION)

override VF2_DTB_NAMES := $(foreach entry,$(VF2_DTB_FILES),$(word 1,$(subst |, ,$(entry))))

override VF2_DTBS := $(addprefix $(VF2_DTB_DIR)/,$(VF2_DTB_NAMES))

# The size or SHA-256 of a device tree: (1) name, (2) field (2 size, 3 SHA-256).
vf2_dtb_field = $(word $(2),$(subst |, ,$(filter $(1)|%,$(VF2_DTB_FILES))))

# Each device tree is fetched to a temporary name, checked, and then renamed.
$(VF2_DTB_DIR)/%.dtb:
	@mkdir -p $(dir $@)
	curl --fail --location --silent --show-error --retry 3 --retry-all-errors --retry-delay 5 \
		-o $@.tmp '$(VF2_DTB_URL)/$*.dtb'
	@set -eu; \
	size=$$(wc -c < $@.tmp | tr -d '[:space:]'); \
	hash=$$(sha256sum $@.tmp | awk '{print $$1}'); \
	if test "$$size" != '$(call vf2_dtb_field,$*.dtb,2)' || \
	   test "$$hash" != '$(call vf2_dtb_field,$*.dtb,3)'; then \
		echo 'visionfive2-dtb: size or SHA-256 mismatch: $*.dtb' >&2; \
		rm -f $@.tmp; \
		exit 1; \
	fi
	@mv -f $@.tmp $@

.PHONY: download visionfive2-dtb
download: visionfive2-dtb
visionfive2-dtb: $(VF2_DTBS)

firmware-platform: $(VF2_DTBS)
