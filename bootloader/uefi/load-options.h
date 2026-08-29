/* Bounded UEFI LoadOptions to ASCII parameter record conversion. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UEFI_LOAD_OPTIONS_H
#define ZEDBSD_UEFI_LOAD_OPTIONS_H

#include <stddef.h>
#include <stdint.h>
#include "bootloader/include/boot-parameter-handoff.h"

enum zbl_uefi_load_options_result {
	ZBL_UEFI_LOAD_OPTIONS_OK = 0,
	ZBL_UEFI_LOAD_OPTIONS_INVALID_ARGUMENT,
	ZBL_UEFI_LOAD_OPTIONS_EMPTY,
	ZBL_UEFI_LOAD_OPTIONS_ODD_SIZE,
	ZBL_UEFI_LOAD_OPTIONS_TOO_LONG,
	ZBL_UEFI_LOAD_OPTIONS_MISSING_NUL,
	ZBL_UEFI_LOAD_OPTIONS_EMBEDDED_NUL,
	ZBL_UEFI_LOAD_OPTIONS_NON_ASCII,
	ZBL_UEFI_LOAD_OPTIONS_UNRECOGNIZED,
	ZBL_UEFI_LOAD_OPTIONS_DESCRIPTOR
};

/*
 * DESCRIPTOR is a successful result: record contains either the recognized
 * OptionalData text or the image default when OptionalData was empty.
 */
enum zbl_uefi_load_options_result zbl_uefi_load_options_record(
	struct zedbsd_boot_parameter_record *record,
	const void *load_options, uint32_t load_options_size,
	const char *image_parameters, size_t image_parameter_capacity);

const char *zbl_uefi_load_options_result_name(
	enum zbl_uefi_load_options_result result);

#endif
