/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The UEFI loader's reading of files on the root partition in zedbsd.cfg.
 *
 * kernel=rootpart:PATH and logo=rootpart:PATH name a file by its absolute
 * path on the UFS root partition that rootpart=PARTUUID=GUID names, rather
 * than on the FAT volume that holds zedbsd.cfg.  The split takes the
 * rootpart: prefix off those two lines, so the shared parser checks the
 * remaining path by its usual rules, and reports which files are on the
 * root and the root's PARTUUID.
 */

#ifndef KERN_BOOTLOADER_UEFI_ROOT_CONFIG_H
#define KERN_BOOTLOADER_UEFI_ROOT_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* The outcome of the split; every value but OK stops the boot. */
enum zbl_uefi_root_config_result {
	ZBL_UEFI_ROOT_CONFIG_OK = 0,
	ZBL_UEFI_ROOT_CONFIG_INVALID_ARGUMENT,
	ZBL_UEFI_ROOT_CONFIG_NO_ROOTPART,
	ZBL_UEFI_ROOT_CONFIG_DUPLICATE_ROOTPART,
	ZBL_UEFI_ROOT_CONFIG_ROOTPART_NOT_PARTUUID,
	ZBL_UEFI_ROOT_CONFIG_BAD_PARTUUID
};

/*
 * Which files zedbsd.cfg puts on the root partition, and which partition
 * that is.
 *
 * partuuid is in GPT's on-disk byte order (the first three fields little
 * endian), as a device path's HD() node holds it.  It is valid only when
 * one of the files is on the root.
 */
struct zbl_uefi_root_config {
	int kernel_on_root;
	int logo_on_root;
	uint8_t partuuid[16];
};

enum zbl_uefi_root_config_result zbl_uefi_root_config_split(const void *source, size_t source_size, void *output, size_t output_capacity, size_t *output_size, struct zbl_uefi_root_config *root);
const char *zbl_uefi_root_config_result_name(enum zbl_uefi_root_config_result result);

#endif
