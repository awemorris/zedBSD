# zedBSD unified PC-98/PC-AT BIOS and x64 UEFI disk-image platform.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

# This platform composes the native kernels and loaders through the shared
# bootloader/unified rules; it does not define another kernel architecture.
all: hdd-image

hdd-image: unified-hdd-image
	@echo "Unified PC-98/PC-AT BIOS, x64 UEFI, and Pi 4 image: $(BUILD)/hdd-image.img"

.PHONY: all hdd-image
