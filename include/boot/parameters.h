/* Shared loader/kernel boot-parameter transport limits. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_BOOT_PARAMETERS_H
#define KERN_BOOT_PARAMETERS_H

#define KERN_BOOT_PARAMETERS_TEXT_MAX 3071
#define KERN_BOOT_PARAMETERS_STORAGE_SIZE \
	(KERN_BOOT_PARAMETERS_TEXT_MAX + 1)

#define KERN_IMAGE_BOOT_PARAMETERS_TEXT \
	"overlay-root=boot0:rootfs.img overlay-data=boot0:data.img " \
	"swap0=boot0:swapfile"

#define KERN_BOOT_PARAMETERS_DEFAULT_TEXT \
	KERN_IMAGE_BOOT_PARAMETERS_TEXT

#endif
