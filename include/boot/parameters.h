/* Shared loader/kernel boot-parameter transport limits. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BOOT_PARAMETERS_H
#define ZEDBSD_BOOT_PARAMETERS_H

#define ZEDBSD_BOOT_PARAMETERS_TEXT_MAX 3071
#define ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE \
	(ZEDBSD_BOOT_PARAMETERS_TEXT_MAX + 1)

#define ZEDBSD_BOOT_PARAMETERS_DEFAULT_TEXT \
	"overlay-root=boot0:rootfs.img overlay-data=boot0:data.img " \
	"swap0=boot0:swapfile"

#endif
