/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Architecture-independent early boot parameters.
 */

#ifndef ZEDBSD_KERN_BOOT_PARAMETERS_H
#define ZEDBSD_KERN_BOOT_PARAMETERS_H

#include <stddef.h>
#include <stdint.h>
#include <boot/parameters.h>

#define KERN_BOOT_PARAMETERS_TEXT_MAX ZEDBSD_BOOT_PARAMETERS_TEXT_MAX
#define KERN_BOOT_PARAMETERS_STORAGE_SIZE ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE
#define KERN_BOOT_PARAMETERS_INIT_PATH_MAX 255U
#define KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX 31U
#define KERN_BOOT_PARAMETER_OFFSET_ABSENT UINT16_MAX

enum kern_boot_parameter_key {
	KERN_BOOT_PARAMETER_BOOT0,
	KERN_BOOT_PARAMETER_BOOT1,
	KERN_BOOT_PARAMETER_BOOT2,
	KERN_BOOT_PARAMETER_BOOT3,
	KERN_BOOT_PARAMETER_ROOTPART,
	KERN_BOOT_PARAMETER_OVERLAY_ROOT,
	KERN_BOOT_PARAMETER_OVERLAY_DATA,
	KERN_BOOT_PARAMETER_SWAP0,
	KERN_BOOT_PARAMETER_SWAP1,
	KERN_BOOT_PARAMETER_SWAP2,
	KERN_BOOT_PARAMETER_SWAP3,
	KERN_BOOT_PARAMETER_INIT,
	KERN_BOOT_PARAMETER_COUNT
};

/*
 * Values are offsets into storage so the structure remains self-contained
 * when a host fixture or a future handoff path copies it.
 */
struct kern_boot_parameters {
	char storage[KERN_BOOT_PARAMETERS_STORAGE_SIZE];
	uint16_t value_offset[KERN_BOOT_PARAMETER_COUNT];
	unsigned unknown_count;
	unsigned unknown_name_truncated;
	char unknown_name[KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX + 1U];
};

/*
 * Parse at most input_capacity readable bytes, including the terminating NUL.
 * A NULL input with zero capacity denotes an empty parameter set.  On error,
 * parameters is reset to an empty, safely inspectable result.
 *
 * EINVAL       invalid arguments, syntax, control data, or relative init path
 * EILSEQ       a non-ASCII byte before the terminating NUL
 * E2BIG        text exceeds 3071 bytes or lacks NUL at that maximum boundary
 * EEXIST       a known name occurs more than once
 * ENAMETOOLONG init path is 256 bytes or longer
 */
int
kern_boot_parameters_parse(
	struct kern_boot_parameters *parameters,
	const char *input,
	size_t input_capacity);

const char *
kern_boot_parameters_value(
	const struct kern_boot_parameters *parameters,
	enum kern_boot_parameter_key key);

const char *
kern_boot_parameters_boot(
	const struct kern_boot_parameters *parameters,
	unsigned index);

const char *
kern_boot_parameters_swap(
	const struct kern_boot_parameters *parameters,
	unsigned index);

const char *
kern_boot_parameters_rootpart(
	const struct kern_boot_parameters *parameters);

const char *
kern_boot_parameters_overlay_root(
	const struct kern_boot_parameters *parameters);

const char *
kern_boot_parameters_overlay_data(
	const struct kern_boot_parameters *parameters);

const char *
kern_boot_parameters_init_path(
	const struct kern_boot_parameters *parameters);

unsigned
kern_boot_parameters_unknown_count(
	const struct kern_boot_parameters *parameters);

const char *
kern_boot_parameters_unknown_name(
	const struct kern_boot_parameters *parameters,
	int *truncated);

/* Kernel-global parse-once instance consumed by init and later VFS phases. */
int
kern_boot_parameters_initialize(
	const char *input,
	size_t input_capacity);

const struct kern_boot_parameters *
kern_boot_parameters_current(void);

/*
 * True only when the valid kernel-global instance was initialized from an
 * actual parameter source.  This deliberately distinguishes an absent source
 * (NULL, zero capacity) from a present but empty string.
 */
int
kern_boot_parameters_source_present(void);

#endif
