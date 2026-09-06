/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Swap sources named on the boot command line.
 *
 * Each swap parameter is either a boot source reference of the form
 * boot<slot>:<path>, prepared as a swap file, or a device selector,
 * resolved through the block identity and prepared as raw swap.  The
 * first failure aborts the whole set and reports which parameter failed.
 */

#include <kern/swap-boot.h>

#include <kern/block-identity.h>
#include <kern/boot.h>
#include <kern/disk.h>
#include <kern/mount.h>

#include <errno.h>
#include <string.h>

static int is_boot_reference(const char *value);

/*
 * Prepares the swap source set from the boot parameters.
 *
 * On failure the set is aborted and failed_parameter names the parameter
 * that could not be prepared.
 */
int
kern_swap_boot_prepare(
	const struct kern_boot_parameters *parameters,
	struct kern_boot_source_context *boot_sources,
	struct kern_swap_source_set *swap_sources,
	unsigned *failed_parameter)
{
	struct kern_swap_source source;
	struct path path;
	struct disk *disk;
	const char *value;
	unsigned parameter;
	unsigned boot_slot;
	int error;

	error = 0;

	/* Rejects a missing operand. */
	if (parameters == NULL || boot_sources == NULL || swap_sources == NULL)
		return EINVAL;

	/* Starts with an empty set and no failed parameter. */
	kern_swap_source_set_init(swap_sources);
	if (failed_parameter != NULL)
		*failed_parameter = KERN_SWAP_SOURCE_COUNT;

	/* Prepares each named source in parameter order. */
	for (parameter = 0; parameter < KERN_SWAP_SOURCE_COUNT; parameter++) {
		value = kern_boot_parameters_swap(parameters, parameter);
		if (value == NULL)
			continue;
		kern_swap_source_init(&source);

		/* A boot reference is a swap file on a boot source. */
		if (is_boot_reference(value)) {
			path_init(&path);
			error = kern_boot_source_lookup(boot_sources, value,
			    &boot_slot, &path);
			if (error == 0)
				error = kern_swap_source_prepare_file(&path,
				    parameter, &source);
			path_release(&path);
			if (error == 0)
				error = kern_swap_source_set_diagnostic(&source, value);
			if (error == 0)
				error = kern_swap_source_set_add(swap_sources,
				    &source);
			if (error == 0)
				error = kern_boot_source_retain_slot(boot_sources,
				    boot_slot);
		} else {
			/* Anything else selects a disk used as raw swap. */
			disk = NULL;
			error = kern_boot_source_selector_validate(value);
			if (error == 0)
				error = block_identity_resolve(value, &disk);
			if (error == 0)
				error = kern_swap_source_prepare_raw(disk, parameter,
				    &source);
			if (disk != NULL)
				disk_release(disk);
			if (error == 0)
				error = kern_swap_source_set_diagnostic(&source, value);
			if (error == 0)
				error = kern_swap_source_set_add(swap_sources,
				    &source);
		}

		/* The first failure abandons the whole set. */
		if (error != 0) {
			kern_swap_source_destroy(&source);
			if (failed_parameter != NULL)
				*failed_parameter = parameter;
			(void)kern_swap_source_set_abort(swap_sources);
			return error;
		}
	}

	/* Reports the prepared set. */
	return 0;
}

/* Tests whether a swap parameter is a boot<slot>:<path> reference. */
static int
is_boot_reference(
	const char *value)
{
	/* A reference starts with "boot" and carries a colon. */
	if (value == NULL)
		return 0;
	if (strncmp(value, "boot", 4U) != 0)
		return 0;
	if (strchr(value, ':') == NULL)
		return 0;

	/* Reports a boot reference. */
	return 1;
}
