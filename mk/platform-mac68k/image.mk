# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# 68040 Macintosh disk image build rules
#
# The disk is Apple-partitioned (mk/tools/make-mac68k-image.py): the map,
# a 4 MiB driver area, the HFS boot partition "zedBSD" (the boot block,
# zedboot, vmunix and zedbsd.cfg), the UFS root "zedBSD-root" and swap
# "zedBSD-swap" (both Apple_UNIX_SVR2).  The image that is handed out has
# no disk driver: the user installs Apple's with Apple's tools.  A
# developer may merge a dump of the drivers of his own Apple-initialized
# disk (mk/tools/apm-driver-dump.py), kept outside the repository, by
# naming it in MAC68K_DRIVER_DUMP; such an image is not to be handed on.
#
# The platform has no C library and no root tree yet: the root is a
# minimal tree with the root marker alone, and image-platform is run
# directly (make ... image-platform), not through "make disk".
#

DISK_IMAGE_ARTIFACT := $(BUILD)/hdd-image.img

MAC68K_BOOT_MIB ?= 16
ROOT_MIB ?= 64
SWAP_MIB ?= 64

# The dump of Apple's drivers to merge (a file outside the repository),
# or empty for the image that is handed out.
MAC68K_DRIVER_DUMP ?=

# The driver area of an image without a dump: free, or empty-driver43
# (an empty Apple_Driver43 partition) for trying Apple's tools.
MAC68K_DRIVER_AREA ?= free

MAC68K_ROOT_TREE := $(BUILD)/mac68k-root
MAC68K_IMAGE_OPTIONS_STAMP := $(BUILD)/.mac68k-image-options

# The image's options: the dump, the driver area and the sizes.  The stamp
# is written every time and replaced only when they change.
MAC68K_IMAGE_OPTIONS := \
	$(if $(MAC68K_DRIVER_DUMP),--driver-dump $(MAC68K_DRIVER_DUMP),--driver-area $(MAC68K_DRIVER_AREA)) \
	--boot-mib $(MAC68K_BOOT_MIB) \
	--root-mib $(ROOT_MIB) \
	--swap-mib $(SWAP_MIB)

.PHONY: FORCE_MAC68K_IMAGE_OPTIONS
FORCE_MAC68K_IMAGE_OPTIONS:
$(MAC68K_IMAGE_OPTIONS_STAMP): FORCE_MAC68K_IMAGE_OPTIONS
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(MAC68K_IMAGE_OPTIONS)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

# The minimal root tree: the root marker alone.
$(MAC68K_ROOT_TREE)/etc/zedbsd-root:
	@mkdir -p $(dir $@)
	@umask 022; printf 'zedBSD ufs root v1\n' > $@

$(DISK_IMAGE_ARTIFACT): $(BUILD)/vmunix $(MAC68K_BOOT_BLOCK) $(MAC68K_STAGE2) \
		$(MAC68K_ROOT_TREE)/etc/zedbsd-root \
		mk/platform-mac68k/zedbsd.cfg \
		$(MAC68K_IMAGE_OPTIONS_STAMP) \
		$(MAC68K_DRIVER_DUMP) \
		mk/tools/make-mac68k-image.py \
		mk/tools/apm.py \
		mk/tools/hfs.py mk/tools/ufs_format.py
	$(PYTHON) mk/tools/make-mac68k-image.py --kernel $(BUILD)/vmunix --boot-block $(MAC68K_BOOT_BLOCK) --stage2 $(MAC68K_STAGE2) --config mk/platform-mac68k/zedbsd.cfg --root-tree $(MAC68K_ROOT_TREE) $(MAC68K_IMAGE_OPTIONS) --output $@

.PHONY: disk-image image-platform image-mac68k

disk-image: $(DISK_IMAGE_ARTIFACT)
image-platform: $(DISK_IMAGE_ARTIFACT)
image-mac68k: image-platform
