/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bounded zedbsd.cfg parsing and boot-parameter assembly.
 */

#ifndef KERN_BOOTLOADER_UEFI_KERN_CONFIG_H
#define KERN_BOOTLOADER_UEFI_KERN_CONFIG_H

#include "bootloader/include/boot-parameter-handoff.h"

/* The bounds the parser enforces on the file. */
#define ZBL_KERN_CONFIG_FILE_MAX			4096
#define ZBL_KERN_CONFIG_LINE_MAX			511
#define ZBL_KERN_CONFIG_LINE_COUNT_MAX			64

/* The kernel path's storage and its longest path. */
#define ZBL_KERN_CONFIG_KERNEL_PATH_STORAGE_SIZE	256
#define ZBL_KERN_CONFIG_KERNEL_PATH_MAX			(ZBL_KERN_CONFIG_KERNEL_PATH_STORAGE_SIZE - 1)

/* The length of a FAT volume serial in XXXX-XXXX form. */
#define ZBL_KERN_CONFIG_FAT_UUID_LENGTH			9

/* The result's layout, which the BIOS loaders' assembly reads by offset. */
#define ZBL_KERN_CONFIG_PARAMETER_RECORD_OFFSET 	ZBL_KERN_CONFIG_KERNEL_PATH_STORAGE_SIZE
#define ZBL_KERN_CONFIG_RESULT_SIZE			(ZBL_KERN_CONFIG_PARAMETER_RECORD_OFFSET + KERN_BOOT_PARAMETER_RECORD_SIZE)

#ifndef __ASSEMBLER__

#include <stddef.h>

/*
 * What the parser takes from zedbsd.cfg for one boot.
 *
 * The caller owns it; the parser clears it first and on every failure, so it
 * holds either a complete result or nothing.
 */
struct zbl_uefi_kern_config {
	/* A validated path relative to the selected SimpleFS root. */
	char kernel_path[ZBL_KERN_CONFIG_KERNEL_PATH_STORAGE_SIZE];
	struct kern_boot_parameter_record parameter_record;
};

/* The outcome of parsing zedbsd.cfg; every value other than OK stops the boot. */
enum zbl_uefi_kern_config_result {
	ZBL_UEFI_KERN_CONFIG_OK = 0,
	ZBL_UEFI_KERN_CONFIG_INVALID_ARGUMENT,
	ZBL_UEFI_KERN_CONFIG_FILE_TOO_LONG,
	ZBL_UEFI_KERN_CONFIG_TOO_MANY_LINES,
	ZBL_UEFI_KERN_CONFIG_LINE_TOO_LONG,
	ZBL_UEFI_KERN_CONFIG_INVALID_CHARACTER,
	ZBL_UEFI_KERN_CONFIG_INVALID_LINE_ENDING,
	ZBL_UEFI_KERN_CONFIG_UNSUPPORTED_SYNTAX,
	ZBL_UEFI_KERN_CONFIG_MALFORMED_LINE,
	ZBL_UEFI_KERN_CONFIG_DUPLICATE_KERNEL,
	ZBL_UEFI_KERN_CONFIG_MISSING_KERNEL,
	ZBL_UEFI_KERN_CONFIG_INVALID_KERNEL_PATH,
	ZBL_UEFI_KERN_CONFIG_KERNEL_PATH_TOO_LONG,
	ZBL_UEFI_KERN_CONFIG_INVALID_PARAMETER_PATH,
	ZBL_UEFI_KERN_CONFIG_PARAMETER_PATH_TOO_LONG,
	ZBL_UEFI_KERN_CONFIG_INVALID_SELECTED_UUID,
	ZBL_UEFI_KERN_CONFIG_PARAMETERS_TOO_LONG
};

/*
 * selected_uuid is a bounded, NUL-terminated canonical FAT volume serial in
 * XXXX-XXXX form.  kernel_path has no leading slash on successful return.
 * The result object is cleared on every failure when it is non-NULL.
 */
enum zbl_uefi_kern_config_result
zbl_uefi_kern_config_parse(
	struct zbl_uefi_kern_config *configuration,
	const void *source,
	size_t source_size,
	const char *selected_uuid,
	size_t selected_uuid_capacity);

const char *
zbl_uefi_kern_config_result_name(
	enum zbl_uefi_kern_config_result result);

_Static_assert(sizeof(((struct zbl_uefi_kern_config *)0)->kernel_path) ==
	       ZBL_KERN_CONFIG_KERNEL_PATH_STORAGE_SIZE,
	       "zedbsd config kernel path storage size");
_Static_assert(offsetof(struct zbl_uefi_kern_config, parameter_record) ==
	       ZBL_KERN_CONFIG_PARAMETER_RECORD_OFFSET,
	       "zedbsd config parameter record offset");
_Static_assert(sizeof(struct zbl_uefi_kern_config) ==
	       ZBL_KERN_CONFIG_RESULT_SIZE,
	       "zedbsd config result size");

#endif

#endif
