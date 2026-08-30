/* Bounded zedbsd.cfg parsing and boot-parameter assembly. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_BOOTLOADER_UEFI_ZEDBSD_CONFIG_H
#define ZEDBSD_BOOTLOADER_UEFI_ZEDBSD_CONFIG_H

#include "bootloader/include/boot-parameter-handoff.h"

#define ZBL_ZEDBSD_CONFIG_FILE_MAX 4096
#define ZBL_ZEDBSD_CONFIG_LINE_MAX 511
#define ZBL_ZEDBSD_CONFIG_LINE_COUNT_MAX 64
#define ZBL_ZEDBSD_CONFIG_KERNEL_PATH_STORAGE_SIZE 256
#define ZBL_ZEDBSD_CONFIG_KERNEL_PATH_MAX \
	(ZBL_ZEDBSD_CONFIG_KERNEL_PATH_STORAGE_SIZE - 1)
#define ZBL_ZEDBSD_CONFIG_FAT_UUID_LENGTH 9
#define ZBL_ZEDBSD_CONFIG_PARAMETER_RECORD_OFFSET \
	ZBL_ZEDBSD_CONFIG_KERNEL_PATH_STORAGE_SIZE
#define ZBL_ZEDBSD_CONFIG_RESULT_SIZE \
	(ZBL_ZEDBSD_CONFIG_PARAMETER_RECORD_OFFSET + \
	 ZEDBSD_BOOT_PARAMETER_RECORD_SIZE)

#ifndef __ASSEMBLER__
#include <stddef.h>

struct zbl_uefi_zedbsd_config {
	/* A validated path relative to the selected SimpleFS root. */
	char kernel_path[ZBL_ZEDBSD_CONFIG_KERNEL_PATH_STORAGE_SIZE];
	struct zedbsd_boot_parameter_record parameter_record;
};

_Static_assert(sizeof(((struct zbl_uefi_zedbsd_config *)0)->kernel_path) ==
	       ZBL_ZEDBSD_CONFIG_KERNEL_PATH_STORAGE_SIZE,
	       "zedbsd config kernel path storage size");
_Static_assert(offsetof(struct zbl_uefi_zedbsd_config, parameter_record) ==
	       ZBL_ZEDBSD_CONFIG_PARAMETER_RECORD_OFFSET,
	       "zedbsd config parameter record offset");
_Static_assert(sizeof(struct zbl_uefi_zedbsd_config) ==
	       ZBL_ZEDBSD_CONFIG_RESULT_SIZE,
	       "zedbsd config result size");

enum zbl_uefi_zedbsd_config_result {
	ZBL_UEFI_ZEDBSD_CONFIG_OK = 0,
	ZBL_UEFI_ZEDBSD_CONFIG_INVALID_ARGUMENT,
	ZBL_UEFI_ZEDBSD_CONFIG_FILE_TOO_LONG,
	ZBL_UEFI_ZEDBSD_CONFIG_TOO_MANY_LINES,
	ZBL_UEFI_ZEDBSD_CONFIG_LINE_TOO_LONG,
	ZBL_UEFI_ZEDBSD_CONFIG_INVALID_CHARACTER,
	ZBL_UEFI_ZEDBSD_CONFIG_INVALID_LINE_ENDING,
	ZBL_UEFI_ZEDBSD_CONFIG_UNSUPPORTED_SYNTAX,
	ZBL_UEFI_ZEDBSD_CONFIG_MALFORMED_LINE,
	ZBL_UEFI_ZEDBSD_CONFIG_DUPLICATE_KERNEL,
	ZBL_UEFI_ZEDBSD_CONFIG_MISSING_KERNEL,
	ZBL_UEFI_ZEDBSD_CONFIG_INVALID_KERNEL_PATH,
	ZBL_UEFI_ZEDBSD_CONFIG_KERNEL_PATH_TOO_LONG,
	ZBL_UEFI_ZEDBSD_CONFIG_INVALID_PARAMETER_PATH,
	ZBL_UEFI_ZEDBSD_CONFIG_PARAMETER_PATH_TOO_LONG,
	ZBL_UEFI_ZEDBSD_CONFIG_INVALID_SELECTED_UUID,
	ZBL_UEFI_ZEDBSD_CONFIG_PARAMETERS_TOO_LONG
};

/*
 * selected_uuid is a bounded, NUL-terminated canonical FAT volume serial in
 * XXXX-XXXX form.  kernel_path has no leading slash on successful return.
 * The result object is cleared on every failure when it is non-NULL.
 */
enum zbl_uefi_zedbsd_config_result zbl_uefi_zedbsd_config_parse(
	struct zbl_uefi_zedbsd_config *configuration,
	const void *source, size_t source_size,
	const char *selected_uuid, size_t selected_uuid_capacity);

const char *zbl_uefi_zedbsd_config_result_name(
	enum zbl_uefi_zedbsd_config_result result);
#endif

#endif
