/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Early x86 boot-parameter record validation and kernel-owned storage.
 */

#ifndef KERN_HAL_X86_BOOT_PARAMETERS_H
#define KERN_HAL_X86_BOOT_PARAMETERS_H

#include <stddef.h>
#include <kern/boot.h>

enum x86_boot_parameters_result {
	X86_BOOT_PARAMETERS_OK = 0,
	X86_BOOT_PARAMETERS_INVALID_ARGUMENT,
	X86_BOOT_PARAMETERS_INVALID_RECORD,
	X86_BOOT_PARAMETERS_UNTERMINATED,
	X86_BOOT_PARAMETERS_NON_ASCII
};

enum x86_boot_parameters_result
x86_boot_parameters_copy(
	char destination[KERN_BOOT_PARAMETERS_STORAGE_SIZE],
	const char *source,
	size_t source_capacity);

enum x86_boot_parameters_result
x86_boot_parameter_record_copy(
	char destination[KERN_BOOT_PARAMETERS_STORAGE_SIZE],
	const struct kern_boot_parameter_record *record,
	size_t available);

#endif
