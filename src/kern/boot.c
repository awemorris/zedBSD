/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/boot.h"

#include "kern/block-identity.h"
#include "kern/fat-vfs.h"
#include "kern/fat.h"
#include "kern/namei.h"

#include <errno.h>
#include <string.h>

struct parameter_name {
	const char *text;
	size_t length;
};

#define PARAMETER_NAME(value) { value, sizeof(value) - 1U }

static const struct parameter_name parameter_names[KERN_BOOT_PARAMETER_COUNT] = {
	PARAMETER_NAME("boot0"),
	PARAMETER_NAME("boot1"),
	PARAMETER_NAME("boot2"),
	PARAMETER_NAME("boot3"),
	PARAMETER_NAME("rootpart"),
	PARAMETER_NAME("overlay-root"),
	PARAMETER_NAME("overlay-data"),
	PARAMETER_NAME("swap0"),
	PARAMETER_NAME("swap1"),
	PARAMETER_NAME("swap2"),
	PARAMETER_NAME("swap3"),
	PARAMETER_NAME("init"),
};

static struct kern_boot_parameters current_parameters;
static int current_parameters_valid;
static int current_parameters_source_present;

static void
parameters_reset(struct kern_boot_parameters *parameters)
{
	unsigned index;

	parameters->storage[0] = '\0';
	for (index = 0; index < KERN_BOOT_PARAMETER_COUNT; index++)
		parameters->value_offset[index] =
		    KERN_BOOT_PARAMETER_OFFSET_ABSENT;
	parameters->unknown_count = 0;
	parameters->unknown_name_truncated = 0;
	parameters->unknown_name[0] = '\0';
}

static int
parse_error(struct kern_boot_parameters *parameters, int error)
{
	parameters_reset(parameters);
	return error;
}

static int
name_matches(const char *name, size_t length,
	     const struct parameter_name *candidate)
{
	size_t index;

	if (length != candidate->length)
		return 0;
	for (index = 0; index < length; index++)
		if (name[index] != candidate->text[index])
			return 0;
	return 1;
}

static int
parameter_key(const char *name, size_t length,
	      enum kern_boot_parameter_key *key)
{
	unsigned index;

	for (index = 0; index < KERN_BOOT_PARAMETER_COUNT; index++)
		if (name_matches(name, length, &parameter_names[index])) {
			*key = (enum kern_boot_parameter_key)index;
			return 1;
		}
	return 0;
}

static void
record_unknown(struct kern_boot_parameters *parameters, const char *name,
	       size_t length)
{
	size_t copy_length;

	parameters->unknown_count++;
	if (parameters->unknown_count != 1U)
		return;
	copy_length = length;
	if (copy_length > KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX) {
		copy_length = KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX;
		parameters->unknown_name_truncated = 1;
	}
	for (size_t index = 0; index < copy_length; index++)
		parameters->unknown_name[index] = name[index];
	parameters->unknown_name[copy_length] = '\0';
}

int
kern_boot_parameters_parse(struct kern_boot_parameters *parameters,
			   const char *input, size_t input_capacity)
{
	size_t length = 0;
	size_t scan_limit;
	size_t position;
	int terminated = 0;

	if (parameters == NULL)
		return EINVAL;
	parameters_reset(parameters);
	if (input == NULL)
		return input_capacity == 0U ? 0 : EINVAL;
	if (input_capacity == 0U)
		return EINVAL;

	scan_limit = input_capacity;
	if (scan_limit > KERN_BOOT_PARAMETERS_STORAGE_SIZE)
		scan_limit = KERN_BOOT_PARAMETERS_STORAGE_SIZE;
	for (length = 0; length < scan_limit; length++) {
		unsigned char byte = (unsigned char)input[length];

		if (byte == 0U) {
			terminated = 1;
			break;
		}
		if (byte > 0x7fU)
			return parse_error(parameters, EILSEQ);
	}
	if (!terminated)
		return parse_error(parameters,
		    input_capacity >= KERN_BOOT_PARAMETERS_STORAGE_SIZE ? E2BIG :
								 EINVAL);
	for (position = 0; position <= length; position++)
		parameters->storage[position] = input[position];

	position = 0;
	while (position < length) {
		size_t token_start;
		size_t token_end;
		size_t next;
		size_t equal;
		size_t value_start;
		size_t value_length;
		enum kern_boot_parameter_key key;
		int known;

		while (position < length &&
		       parameters->storage[position] == ' ')
			position++;
		if (position == length)
			break;
		token_start = position;
		while (position < length &&
		       parameters->storage[position] != ' ') {
			unsigned char byte =
			    (unsigned char)parameters->storage[position];

			if (byte < 0x21U || byte > 0x7eU)
				return parse_error(parameters, EINVAL);
			position++;
		}
		token_end = position;
		next = token_end;
		while (next < length && parameters->storage[next] == ' ')
			next++;
		equal = token_start;
		while (equal < token_end && parameters->storage[equal] != '=')
			equal++;
		if (equal == token_start || equal == token_end ||
		    equal + 1U == token_end)
			return parse_error(parameters, EINVAL);

		known = parameter_key(parameters->storage + token_start,
		    equal - token_start, &key);
		value_start = equal + 1U;
		value_length = token_end - value_start;
		if (known &&
		    parameters->value_offset[key] !=
			KERN_BOOT_PARAMETER_OFFSET_ABSENT)
			return parse_error(parameters, EEXIST);
		if (known && key == KERN_BOOT_PARAMETER_INIT) {
			if (parameters->storage[value_start] != '/')
				return parse_error(parameters, EINVAL);
			if (value_length > KERN_BOOT_PARAMETERS_INIT_PATH_MAX)
				return parse_error(parameters, ENAMETOOLONG);
		}

		if (token_end < length)
			parameters->storage[token_end] = '\0';
		parameters->storage[equal] = '\0';
		if (known)
			parameters->value_offset[key] = (uint16_t)value_start;
		else
			record_unknown(parameters,
			    parameters->storage + token_start,
			    equal - token_start);
		position = next;
	}
	return 0;
}

const char *
kern_boot_parameters_value(const struct kern_boot_parameters *parameters,
			   enum kern_boot_parameter_key key)
{
	uint16_t offset;

	if (parameters == NULL || (unsigned)key >= KERN_BOOT_PARAMETER_COUNT)
		return NULL;
	offset = parameters->value_offset[key];
	if (offset == KERN_BOOT_PARAMETER_OFFSET_ABSENT)
		return NULL;
	return parameters->storage + offset;
}

const char *
kern_boot_parameters_boot(const struct kern_boot_parameters *parameters,
			 unsigned index)
{
	if (index >= 4U)
		return NULL;
	return kern_boot_parameters_value(parameters,
	    (enum kern_boot_parameter_key)(KERN_BOOT_PARAMETER_BOOT0 + index));
}

const char *
kern_boot_parameters_swap(const struct kern_boot_parameters *parameters,
			 unsigned index)
{
	if (index >= 4U)
		return NULL;
	return kern_boot_parameters_value(parameters,
	    (enum kern_boot_parameter_key)(KERN_BOOT_PARAMETER_SWAP0 + index));
}

const char *
kern_boot_parameters_rootpart(const struct kern_boot_parameters *parameters)
{
	return kern_boot_parameters_value(parameters,
	    KERN_BOOT_PARAMETER_ROOTPART);
}

const char *
kern_boot_parameters_overlay_root(
	const struct kern_boot_parameters *parameters)
{
	return kern_boot_parameters_value(parameters,
	    KERN_BOOT_PARAMETER_OVERLAY_ROOT);
}

const char *
kern_boot_parameters_overlay_data(
	const struct kern_boot_parameters *parameters)
{
	return kern_boot_parameters_value(parameters,
	    KERN_BOOT_PARAMETER_OVERLAY_DATA);
}

const char *
kern_boot_parameters_init_path(const struct kern_boot_parameters *parameters)
{
	const char *path = kern_boot_parameters_value(parameters,
	    KERN_BOOT_PARAMETER_INIT);

	return path != NULL ? path : "/sbin/init";
}

unsigned
kern_boot_parameters_unknown_count(
	const struct kern_boot_parameters *parameters)
{
	return parameters != NULL ? parameters->unknown_count : 0U;
}

const char *
kern_boot_parameters_unknown_name(
	const struct kern_boot_parameters *parameters, int *truncated)
{
	if (truncated != NULL)
		*truncated = parameters != NULL &&
		    parameters->unknown_name_truncated != 0U;
	if (parameters == NULL || parameters->unknown_count == 0U)
		return NULL;
	return parameters->unknown_name;
}

int
kern_boot_parameters_initialize(const char *input, size_t input_capacity)
{
	int error = kern_boot_parameters_parse(&current_parameters, input,
	    input_capacity);

	current_parameters_valid = error == 0;
	current_parameters_source_present = error == 0 && input != NULL;
	return error;
}

const struct kern_boot_parameters *
kern_boot_parameters_current(void)
{
	return current_parameters_valid ? &current_parameters : NULL;
}

int
kern_boot_parameters_source_present(void)
{
	return current_parameters_valid && current_parameters_source_present;
}

/* Pure boot-source grammar helpers.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

static int
selector_text(const char *text, size_t maximum, int device_name)
{
	size_t length = 0;

	if (text == NULL || text[0] == '\0')
		return EINVAL;
	while (text[length] != '\0') {
		unsigned char byte = (unsigned char)text[length];

		if (byte < 0x21U || byte > 0x7eU || byte == '/' ||
		    (device_name && byte == '='))
			return EINVAL;
		if (++length >= maximum)
			return ENAMETOOLONG;
	}
	return 0;
}

int
kern_boot_source_selector_validate(const char *selector)
{
	static const struct {
		const char *prefix;
		size_t length;
	} identities[] = {
		{ "UUID=", 5U },
		{ "LABEL=", 6U },
		{ "PARTUUID=", 9U },
		{ "PARTLABEL=", 10U },
	};
	unsigned index;

	if (selector == NULL || selector[0] == '\0')
		return EINVAL;
	if (strncmp(selector, "/dev/", 5U) == 0)
		return selector_text(selector + 5U, DISK_NAME_MAX, 1);
	for (index = 0; index < sizeof(identities) / sizeof(identities[0]);
	     index++)
		if (strncmp(selector, identities[index].prefix,
		    identities[index].length) == 0)
			return selector_text(selector + identities[index].length,
			    DISK_IDENTITY_TEXT_MAX, 0);
	if (strchr(selector, '=') != NULL)
		return EINVAL;
	return selector_text(selector, DISK_NAME_MAX, 1);
}

int
kern_boot_source_reference_parse(const char *text,
				 struct kern_boot_source_reference *reference)
{
	const char *path;
	size_t length = 0, component = 0;

	if (text == NULL || reference == NULL || text[0] != 'b' ||
	    text[1] != 'o' || text[2] != 'o' || text[3] != 't' ||
	    text[4] < '0' ||
	    text[4] > '3' || text[5] != ':')
		return EINVAL;
	path = text + 6U;
	if (*path == '/')
		path++;
	if (*path == '\0')
		return EINVAL;
	for (;;) {
		unsigned char byte = (unsigned char)path[length];

		if (byte == '\0' || byte == '/') {
			if (length == component ||
			    (length - component == 1U && path[component] == '.') ||
			    (length - component == 2U && path[component] == '.' &&
			     path[component + 1U] == '.'))
				return EINVAL;
			if (byte == '\0')
				break;
			component = length + 1U;
		} else if (byte < 0x20U || byte > 0x7eU || byte == '\\')
			return EINVAL;
		length++;
		if (length >= sizeof(reference->relative))
			return ENAMETOOLONG;
	}
	reference->slot = (unsigned)(text[4] - '0');
	memcpy(reference->relative, path, length + 1U);
	return 0;
}

int
kern_boot_source_root_mode(const char *rootpart, const char *overlay_root,
			  const char *overlay_data,
			  enum kern_boot_root_mode *mode)
{
	if (mode == NULL)
		return EINVAL;
	*mode = KERN_BOOT_ROOT_INVALID;
	if (rootpart != NULL) {
		if (overlay_root != NULL || overlay_data != NULL)
			return EINVAL;
		*mode = KERN_BOOT_ROOT_NATIVE;
		return 0;
	}
	if (overlay_root == NULL || overlay_data == NULL)
		return EINVAL;
	*mode = KERN_BOOT_ROOT_OVERLAY;
	return 0;
}

int
kern_boot_source_fat_type_supported(enum bootfat_type type)
{
	return type == ZEDBSD_FAT16 || type == ZEDBSD_FAT32;
}

const char *
kern_boot_source_failure_stage_name(enum kern_boot_source_failure_stage stage)
{
	switch (stage) {
	case KERN_BOOT_SOURCE_FAILURE_SELECTOR:
		return "selector validation";
	case KERN_BOOT_SOURCE_FAILURE_RESOLVE:
		return "selector resolution";
	case KERN_BOOT_SOURCE_FAILURE_PARTITION:
		return "partition validation";
	case KERN_BOOT_SOURCE_FAILURE_DUPLICATE:
		return "duplicate detection";
	case KERN_BOOT_SOURCE_FAILURE_FILESYSTEM:
		return "FAT16/FAT32 validation";
	case KERN_BOOT_SOURCE_FAILURE_MOUNT:
		return "private mount";
	default:
		return "unknown stage";
	}
}

/* Private boot-filesystem slot ownership.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

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
	/* A published context is an immutable system-lifetime resolver. */
	if (context->runtime_published)
		return EBUSY;
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
	if (context->runtime_published)
		return EBUSY;
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
		context->slot[slot].runtime_mount =
		    context->slot[slot].mount;
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
kern_boot_source_retain_configured(struct kern_boot_source_context *context)
{
	unsigned slot;

	if (context == NULL || context->runtime_published)
		return EINVAL;
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		struct kern_boot_source_slot *source = &context->slot[slot];

		if (!source->configured)
			continue;
		if (source->mount == NULL || source->runtime_mount == NULL ||
		    source->disk == NULL)
			return EINVAL;
	}
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++)
		if (context->slot[slot].configured)
			context->slot[slot].retained = 1U;
	return 0;
}

int
kern_boot_source_publish_runtime(struct kern_boot_source_context *context)
{
	unsigned slot;

	if (context == NULL || context->runtime_published)
		return EINVAL;
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		const struct kern_boot_source_slot *source = &context->slot[slot];

		if (!source->configured)
			continue;
		if (!source->retained || source->runtime_mount == NULL ||
		    source->disk == NULL)
			return EINVAL;
	}
	/* kern_vfs_init publishes before starting the first userspace process. */
	context->runtime_published = 1U;
	return 0;
}

static int
runtime_mount_lookup(struct kern_boot_source_slot *source,
		     const char *relative, struct path *result)
{
	struct cwdinfo context;
	struct path root;
	int error;

	if (!source->promoted)
		return mount_private_lookup(source->runtime_mount, relative,
		    result);
	path_init(&root);
	path_set(&root, source->runtime_mount, source->runtime_mount->m_root);
	error = cwdinfo_init(&context, &root);
	path_release(&root);
	if (error != 0)
		return error;
	error = namei_path_at(&context, relative, result);
	cwdinfo_destroy(&context);
	return error;
}

int
kern_boot_source_runtime_lookup(struct kern_boot_source_context *context,
				const char *text, struct path *path_out)
{
	struct kern_boot_source_reference reference;
	struct kern_boot_source_slot *source;
	int error;

	if (context == NULL || path_out == NULL)
		return EINVAL;
	path_init(path_out);
	if (!context->runtime_published)
		return ENXIO;
	error = kern_boot_source_reference_parse(text, &reference);
	if (error != 0)
		return error;
	source = &context->slot[reference.slot];
	if (!source->configured || !source->retained ||
	    source->runtime_mount == NULL)
		return ENOENT;
	return runtime_mount_lookup(source, reference.relative, path_out);
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

