/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/swap-boot.h>

#include <kern/block-identity.h>
#include <kern/boot.h>
#include <kern/disk.h>
#include <kern/mount.h>

#include <errno.h>
#include <string.h>

static int
is_boot_reference(const char *value)
{
	return value != NULL && strncmp(value, "boot", 4U) == 0 &&
	    strchr(value, ':') != NULL;
}

int
kern_swap_boot_prepare(const struct kern_boot_parameters *parameters,
		       struct kern_boot_source_context *boot_sources,
		       struct kern_swap_source_set *swap_sources,
		       unsigned *failed_parameter)
{
	unsigned parameter;
	int error = 0;

	if (parameters == NULL || boot_sources == NULL || swap_sources == NULL)
		return EINVAL;
	kern_swap_source_set_init(swap_sources);
	if (failed_parameter != NULL)
		*failed_parameter = KERN_SWAP_SOURCE_COUNT;
	for (parameter = 0; parameter < KERN_SWAP_SOURCE_COUNT; parameter++) {
		const char *value = kern_boot_parameters_swap(parameters, parameter);
		struct kern_swap_source source;

		if (value == NULL)
			continue;
		kern_swap_source_init(&source);
		if (is_boot_reference(value)) {
			struct path path;
			unsigned boot_slot;

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
			struct disk *disk = NULL;

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
		if (error != 0) {
			kern_swap_source_destroy(&source);
			if (failed_parameter != NULL)
				*failed_parameter = parameter;
			(void)kern_swap_source_set_abort(swap_sources);
			return error;
		}
	}
	return 0;
}
