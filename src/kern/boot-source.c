/* Private boot-filesystem slot ownership.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/boot-source.h>
#include <kern/block-identity.h>
#include <kern/fat-vfs.h>

#include <errno.h>
#include <string.h>

void
kern_boot_source_context_init(struct kern_boot_source_context *context)
{
	if (context != NULL)
		memset(context, 0, sizeof(*context));
}

int
kern_boot_source_context_destroy(struct kern_boot_source_context *context)
{
	int first_error = 0;
	unsigned slot;

	if (context == NULL)
		return EINVAL;
	for (slot = KERN_BOOT_SOURCE_SLOT_COUNT; slot != 0U; slot--) {
		struct kern_boot_source_slot *source = &context->slot[slot - 1U];
		int error;

		if (source->mount == NULL)
			continue;
		error = unmount_private(source->mount);
		if (error != 0) {
			if (first_error == 0)
				first_error = error;
			continue;
		}
		memset(source, 0, sizeof(*source));
	}
	return first_error;
}

static int
context_fail(struct kern_boot_source_context *context, unsigned slot,
	     enum kern_boot_source_failure_stage stage, int error)
{
	context->failure_slot = slot;
	context->failure_stage = stage;
	context->cleanup_error = kern_boot_source_context_destroy(context);
	return error;
}

int
kern_boot_source_context_mount(struct kern_boot_source_context *context,
			       const struct kern_boot_parameters *parameters,
			       struct disk *loader_origin,
			       const char *loader_origin_selector)
{
	unsigned slot;

	if (context == NULL || parameters == NULL)
		return EINVAL;
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++)
		if (context->slot[slot].mount != NULL)
			return EBUSY;
	context->failure_stage = KERN_BOOT_SOURCE_FAILURE_NONE;
	context->cleanup_error = 0;
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		const char *selector = kern_boot_parameters_boot(parameters, slot);
		struct disk *disk = NULL;
		enum bootfat_type fat_type;
		unsigned previous;
		int error;

		if (selector == NULL && slot != 0U)
			continue;
		if (selector != NULL) {
			error = kern_boot_source_selector_validate(selector);
			if (error != 0)
				return context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_SELECTOR, error);
			error = block_identity_resolve(selector, &disk);
			if (error != 0)
				return context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_RESOLVE, error);
		} else if (loader_origin_selector != NULL) {
			error = kern_boot_source_selector_validate(
			    loader_origin_selector);
			if (error != 0)
				return context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_SELECTOR, error);
			error = block_identity_resolve(loader_origin_selector, &disk);
			if (error != 0)
				return context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_RESOLVE, error);
		} else if (loader_origin != NULL) {
			disk = loader_origin;
			disk_ref(disk);
		} else {
			return context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_RESOLVE, ENXIO);
		}
		if ((disk->d_flags & DISK_PARTITION) == 0) {
			disk_release(disk);
			return context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_PARTITION, EINVAL);
		}
		for (previous = 0; previous < slot; previous++)
			if (context->slot[previous].configured &&
			    context->slot[previous].disk->d_dev == disk->d_dev)
				break;
		if (previous != slot) {
			disk_release(disk);
			return context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_DUPLICATE, EEXIST);
		}
		error = fat_probe_type(disk, &fat_type);
		if (error == 0 && !kern_boot_source_fat_type_supported(fat_type))
			error = EOPNOTSUPP;
		if (error != 0) {
			disk_release(disk);
			return context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_FILESYSTEM, error);
		}
		error = mount_private("fat", disk, 0, NULL,
		    &context->slot[slot].mount);
		if (error != 0) {
			disk_release(disk);
			return context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_MOUNT, error);
		}
		context->slot[slot].disk = context->slot[slot].mount->m_disk;
		context->slot[slot].configured = 1U;
		disk_release(disk);
	}
	return 0;
}

int
kern_boot_source_lookup(struct kern_boot_source_context *context,
			const char *text, unsigned *slot_out,
			struct path *path_out)
{
	struct kern_boot_source_reference reference;
	int error;

	if (context == NULL || path_out == NULL)
		return EINVAL;
	path_init(path_out);
	error = kern_boot_source_reference_parse(text, &reference);
	if (error != 0)
		return error;
	if (!context->slot[reference.slot].configured ||
	    context->slot[reference.slot].mount == NULL)
		return ENOENT;
	error = mount_private_lookup(context->slot[reference.slot].mount,
	    reference.relative, path_out);
	if (error == 0 && slot_out != NULL)
		*slot_out = reference.slot;
	return error;
}

int
kern_boot_source_retain_slot(struct kern_boot_source_context *context,
			     unsigned slot)
{
	if (context == NULL || slot >= KERN_BOOT_SOURCE_SLOT_COUNT)
		return EINVAL;
	if (!context->slot[slot].configured ||
	    context->slot[slot].mount == NULL)
		return ENOENT;
	context->slot[slot].retained = 1U;
	return 0;
}

int
kern_boot_source_find_disk(const struct kern_boot_source_context *context,
			   const struct disk *disk, unsigned *slot_out)
{
	unsigned slot;

	if (context == NULL || disk == NULL || slot_out == NULL)
		return EINVAL;
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++)
		if (context->slot[slot].configured &&
		    context->slot[slot].disk != NULL &&
		    context->slot[slot].disk->d_dev == disk->d_dev) {
			*slot_out = slot;
			return 0;
		}
	return ENOENT;
}

int
kern_boot_source_promote_root(struct kern_boot_source_context *context,
			      unsigned slot, struct mount **root_out)
{
	struct kern_boot_source_slot *source;
	int error;

	if (context == NULL || slot >= KERN_BOOT_SOURCE_SLOT_COUNT)
		return EINVAL;
	source = &context->slot[slot];
	if (!source->configured || source->mount == NULL)
		return ENOENT;
	error = mount_private_promote_root(source->mount, root_out);
	if (error != 0)
		return error;
	source->mount = NULL;
	source->disk = NULL;
	source->retained = 1U;
	source->promoted = 1U;
	return 0;
}

int
kern_boot_source_release_unused(struct kern_boot_source_context *context)
{
	int first_error = 0;
	unsigned slot;

	if (context == NULL)
		return EINVAL;
	for (slot = KERN_BOOT_SOURCE_SLOT_COUNT; slot != 0U; slot--) {
		struct kern_boot_source_slot *source = &context->slot[slot - 1U];
		int error;

		if (source->mount == NULL || source->retained)
			continue;
		error = unmount_private(source->mount);
		if (error != 0) {
			if (first_error == 0)
				first_error = error;
			continue;
		}
		memset(source, 0, sizeof(*source));
	}
	return first_error;
}
