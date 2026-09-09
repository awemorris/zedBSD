/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Boot parameters and boot sources.
 *
 * The boot loader hands the kernel a parameter string of space-separated
 * name=value tokens naming the boot partitions, the root, the swap
 * sources, and the init path.  A boot source context mounts each named
 * FAT boot partition privately, resolves boot<n>:<path> references into
 * them, and later publishes the retained mounts for the running system.
 */

#include "kern/boot.h"

#include "kern/block-identity.h"
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

static char firmware_source[ZEDBSD_BOOT_SOURCE_SELECTOR_SIZE];
static char configuration_source[ZEDBSD_BOOT_SOURCE_SELECTOR_SIZE];
static uint64_t configuration_matches;
static int provenance_selector(const struct boot_partition_identity *identity, char *output);

static struct kern_boot_parameters current_parameters;
static int current_parameters_valid;
static int current_parameters_source_present;

static void parameters_reset(struct kern_boot_parameters *parameters);
static int parse_error(struct kern_boot_parameters *parameters, int error);
static int name_matches(const char *name, size_t length, const struct parameter_name *candidate);
static int parameter_key(const char *name, size_t length, enum kern_boot_parameter_key *key);
static void record_unknown(struct kern_boot_parameters *parameters, const char *name, size_t length);
static int selector_text(const char *text, size_t maximum, int device_name);
static int context_fail(struct kern_boot_source_context *context, unsigned slot, enum kern_boot_source_failure_stage stage, int error);
static int runtime_mount_lookup(struct kern_boot_source_slot *source, const char *relative, struct path *result);

/* Copies boot provenance before firmware storage can be reclaimed. */
int
kern_boot_provenance_set(const struct boot_provenance *record)
{
	char firmware[ZEDBSD_BOOT_SOURCE_SELECTOR_SIZE];
	char configuration[ZEDBSD_BOOT_SOURCE_SELECTOR_SIZE];
	int error;

	firmware_source[0] = '\0';
	configuration_source[0] = '\0';
	configuration_matches = 0;
	if (record == NULL)
		return 0;
	if (record->version != ZEDBSD_BOOT_PROVENANCE_VERSION ||
	    record->config_matches == 0 || record->config_matches > 128U)
		return EINVAL;
	error = provenance_selector(&record->firmware, firmware);
	if (error == 0)
		error = provenance_selector(&record->configuration, configuration);
	if (error != 0)
		return error;
	memcpy(firmware_source, firmware, sizeof(firmware_source));
	memcpy(configuration_source, configuration, sizeof(configuration_source));
	configuration_matches = record->config_matches;
	return 0;
}

/* Returns an immutable selector, not the possibly overridden boot0 setting. */
const char *
kern_boot_source_selector(unsigned configuration)
{
	const char *source;

	source = configuration ? configuration_source : firmware_source;
	return source[0] != '\0' ? source : "unavailable";
}

uint64_t
kern_boot_config_matches(void)
{
	return configuration_matches;
}

/*
 * Parses a boot parameter string into a parameter record.
 *
 * The input must be terminated ASCII within the storage size.  Tokens
 * are name=value pairs of printable characters separated by spaces; a
 * known name may appear once, and the first unknown name is remembered
 * for diagnostics.  Any error leaves the record empty.
 */
int
kern_boot_parameters_parse(
	struct kern_boot_parameters *parameters,
	const char *input,
	size_t input_capacity)
{
	enum kern_boot_parameter_key key;
	size_t length;
	size_t scan_limit;
	size_t position;
	size_t token_start;
	size_t token_end;
	size_t next;
	size_t equal;
	size_t value_start;
	size_t value_length;
	unsigned char byte;
	unsigned char token_byte;
	int terminated;
	int known;
	int error;

	length = 0;
	terminated = 0;

	/* Rejects a missing record. */
	if (parameters == NULL)
		return EINVAL;
	parameters_reset(parameters);

	/* No input is fine only when no capacity was claimed for it. */
	if (input == NULL) {
		if (input_capacity == 0U)
			return 0;
		return EINVAL;
	}

	if (input_capacity == 0U)
		return EINVAL;

	/* Finds the terminator within the storage size, refusing non-ASCII. */
	scan_limit = input_capacity;
	if (scan_limit > KERN_BOOT_PARAMETERS_STORAGE_SIZE)
		scan_limit = KERN_BOOT_PARAMETERS_STORAGE_SIZE;
	for (length = 0; length < scan_limit; length++) {
		byte = (unsigned char)input[length];
		if (byte == 0U) {
			terminated = 1;
			break;
		}

		if (byte > 0x7fU) {
			error = parse_error(parameters, EILSEQ);
			return error;
		}
	}

	if (!terminated) {
		if (input_capacity >= KERN_BOOT_PARAMETERS_STORAGE_SIZE)
			error = parse_error(parameters, E2BIG);
		else
			error = parse_error(parameters, EINVAL);
		return error;
	}

	/* Works on a private copy that the tokens are terminated in. */
	for (position = 0; position <= length; position++)
		parameters->storage[position] = input[position];

	/* Walks the tokens. */
	position = 0;
	while (position < length) {
		/* Skips the spaces before the token. */
		while (position < length &&
		       parameters->storage[position] == ' ')
			position++;
		if (position == length)
			break;

		/* Delimits a token of printable characters. */
		token_start = position;
		while (position < length &&
		       parameters->storage[position] != ' ') {
			token_byte =
			    (unsigned char)parameters->storage[position];
			if (token_byte < 0x21U || token_byte > 0x7eU) {
				error = parse_error(parameters, EINVAL);
				return error;
			}

			position++;
		}

		token_end = position;
		next = token_end;
		while (next < length && parameters->storage[next] == ' ')
			next++;

		/* Splits the token at its equals sign; both sides must be non-empty. */
		equal = token_start;
		while (equal < token_end && parameters->storage[equal] != '=')
			equal++;
		if (equal == token_start ||
		    equal == token_end ||
		    equal + 1U == token_end) {
			error = parse_error(parameters, EINVAL);
			return error;
		}

		known = parameter_key(parameters->storage + token_start,
				      equal - token_start,
				      &key);

		/* A known name may appear once. */
		value_start = equal + 1U;
		value_length = token_end - value_start;
		if (known &&
		    parameters->value_offset[key] != KERN_BOOT_PARAMETER_OFFSET_ABSENT) {
			error = parse_error(parameters, EEXIST);
			return error;
		}

		/* The init path must be absolute and bounded. */
		if (known && key == KERN_BOOT_PARAMETER_INIT) {
			if (parameters->storage[value_start] != '/') {
				error = parse_error(parameters, EINVAL);
				return error;
			}

			if (value_length > KERN_BOOT_PARAMETERS_INIT_PATH_MAX) {
				error = parse_error(parameters, ENAMETOOLONG);
				return error;
			}
		}

		/* Terminates the name and the value in place. */
		if (token_end < length)
			parameters->storage[token_end] = '\0';
		parameters->storage[equal] = '\0';

		/* Records the value, or the first unknown name. */
		if (known) {
			parameters->value_offset[key] = (uint16_t)value_start;
		} else {
			record_unknown(parameters,
				       parameters->storage + token_start,
				       equal - token_start);
		}

		position = next;
	}

	/* Reports the parsed record. */
	return 0;
}

/*
 * Reads the value of a parameter, or none when it was not given.
 */
const char *
kern_boot_parameters_value(
	const struct kern_boot_parameters *parameters,
	enum kern_boot_parameter_key key)
{
	uint16_t offset;

	/* Rejects a missing record or an unknown key. */
	if (parameters == NULL || (unsigned)key >= KERN_BOOT_PARAMETER_COUNT)
		return NULL;

	/* An absent parameter has no value. */
	offset = parameters->value_offset[key];
	if (offset == KERN_BOOT_PARAMETER_OFFSET_ABSENT)
		return NULL;

	/* Reports the value in the record's storage. */
	return parameters->storage + offset;
}

/*
 * Reads the selector of a boot partition slot.
 */
const char *
kern_boot_parameters_boot(
	const struct kern_boot_parameters *parameters,
	unsigned index)
{
	const char *value;

	/* Only four boot slots exist. */
	if (index >= 4U)
		return NULL;

	value = kern_boot_parameters_value(
		parameters,
		(enum kern_boot_parameter_key)(KERN_BOOT_PARAMETER_BOOT0 + index));

	/* Reports the selector, or none. */
	return value;
}

/*
 * Reads the selector of a swap source slot.
 */
const char *
kern_boot_parameters_swap(
	const struct kern_boot_parameters *parameters,
	unsigned index)
{
	const char *value;

	/* Only four swap slots exist. */
	if (index >= 4U)
		return NULL;

	value = kern_boot_parameters_value(
		parameters,
		(enum kern_boot_parameter_key)(KERN_BOOT_PARAMETER_SWAP0 + index));

	/* Reports the selector, or none. */
	return value;
}

/*
 * Reads the root partition selector.
 */
const char *
kern_boot_parameters_rootpart(
	const struct kern_boot_parameters *parameters)
{
	const char *value;

	value = kern_boot_parameters_value(
		parameters,
		KERN_BOOT_PARAMETER_ROOTPART);

	/* Reports the selector, or none. */
	return value;
}

/*
 * Reads the overlay root selector.
 */
const char *
kern_boot_parameters_overlay_root(
	const struct kern_boot_parameters *parameters)
{
	const char *value;

	value = kern_boot_parameters_value(
		parameters,
		KERN_BOOT_PARAMETER_OVERLAY_ROOT);

	/* Reports the selector, or none. */
	return value;
}

/*
 * Reads the overlay data selector.
 */
const char *
kern_boot_parameters_overlay_data(
	const struct kern_boot_parameters *parameters)
{
	const char *value;

	value = kern_boot_parameters_value(
		parameters,
		KERN_BOOT_PARAMETER_OVERLAY_DATA);

	/* Reports the selector, or none. */
	return value;
}

/*
 * Reads the init path, defaulting to /sbin/init.
 */
const char *
kern_boot_parameters_init_path(
	const struct kern_boot_parameters *parameters)
{
	const char *path;

	/* Falls back to the default when no path was given. */
	path = kern_boot_parameters_value(parameters, KERN_BOOT_PARAMETER_INIT);
	if (path == NULL)
		return "/sbin/init";

	/* Reports the given path. */
	return path;
}

/*
 * Reports how many unknown parameters were seen.
 */
unsigned
kern_boot_parameters_unknown_count(
	const struct kern_boot_parameters *parameters)
{
	/* A missing record saw nothing. */
	if (parameters == NULL)
		return 0U;

	/* Reports the count. */
	return parameters->unknown_count;
}

/*
 * Reads the first unknown parameter name, noting whether it was truncated.
 */
const char *
kern_boot_parameters_unknown_name(
	const struct kern_boot_parameters *parameters,
	int *truncated)
{
	/* Reports the truncation when asked. */
	if (truncated != NULL) {
		if (parameters != NULL && parameters->unknown_name_truncated != 0U)
			*truncated = 1;
		else
			*truncated = 0;
	}

	/* There is no name without an unknown parameter. */
	if (parameters == NULL || parameters->unknown_count == 0U)
		return NULL;

	/* Reports the remembered name. */
	return parameters->unknown_name;
}

/*
 * Parses the boot parameters the kernel keeps for the whole system.
 */
int
kern_boot_parameters_initialize(
	const char *input,
	size_t input_capacity)
{
	int error;

	/* Parses into the system record and remembers whether it is valid. */
	error = kern_boot_parameters_parse(&current_parameters, input, input_capacity);
	current_parameters_valid = error == 0;
	current_parameters_source_present = error == 0 && input != NULL;

	/* Reports why the parse failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads the system boot parameters, or none before they are valid.
 */
const struct kern_boot_parameters *
kern_boot_parameters_current(
	void)
{
	/* The record exists only after a successful parse. */
	if (!current_parameters_valid)
		return NULL;
	return &current_parameters;
}

/*
 * Tests whether the boot loader supplied a parameter string.
 */
int
kern_boot_parameters_source_present(
	void)
{
	/* Both a valid parse and an actual input are required. */
	if (!current_parameters_valid)
		return 0;
	if (!current_parameters_source_present)
		return 0;

	/* Reports a supplied string. */
	return 1;
}

/*
 * Validates the spelling of a device selector.
 *
 * A selector is a /dev/ name, a UUID=, LABEL=, PARTUUID=, or PARTLABEL=
 * identity, or a bare device name.
 */
int
kern_boot_source_selector_validate(
	const char *selector)
{
	unsigned index;
	int error;

	/* The identity prefixes a selector may carry. */
	static const struct {
		const char *prefix;
		size_t length;
	} identities[] = {
		{ "UUID=", 5U },
		{ "LABEL=", 6U },
		{ "PARTUUID=", 9U },
		{ "PARTLABEL=", 10U },
	};

	/* Rejects a missing or empty selector. */
	if (selector == NULL || selector[0] == '\0')
		return EINVAL;

	/* A device path carries a device name. */
	if (strncmp(selector, "/dev/", 5U) == 0) {
		error = selector_text(selector + 5U, DISK_NAME_MAX, 1);
		return error;
	}

	/* An identity prefix carries identity text. */
	for (index = 0; index < sizeof(identities) / sizeof(identities[0]); index++) {
		if (strncmp(selector,
			    identities[index].prefix,
			    identities[index].length) == 0) {
			error = selector_text(selector + identities[index].length,
					     DISK_IDENTITY_TEXT_MAX, 0);
			return error;
		}
	}

	/* Anything else with an equals sign is an unknown identity. */
	if (strchr(selector, '=') != NULL)
		return EINVAL;

	/* A bare name is a device name. */

	/* Reports why the validation failed. */
	error = selector_text(selector, DISK_NAME_MAX, 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Parses a boot<n>:<path> reference into its slot and relative path.
 *
 * The path may not contain empty, dot, or dot-dot components.
 */
int
kern_boot_source_reference_parse(
	const char *text,
	struct kern_boot_source_reference *reference)
{
	const char *path;
	size_t length;
	size_t component;
	unsigned char byte;

	length = 0;
	component = 0;

	/* Rejects anything but boot<0-3>: followed by a path. */
	if (text == NULL ||
	    reference == NULL ||
	    text[0] != 'b' ||
	    text[1] != 'o' ||
	    text[2] != 'o' ||
	    text[3] != 't' ||
	    text[4] < '0' ||
	    text[4] > '3' ||
	    text[5] != ':')
		return EINVAL;
	path = text + 6U;
	if (*path == '/')
		path++;
	if (*path == '\0')
		return EINVAL;

	/* Checks every component and character of the path. */
	for (;;) {
		byte = (unsigned char)path[length];
		if (byte == '\0' || byte == '/') {
			if (length == component ||
			    (length - component == 1U && path[component] == '.') ||
			    (length - component == 2U &&
			     path[component] == '.' &&
			     path[component + 1U] == '.'))
				return EINVAL;
			if (byte == '\0')
				break;
			component = length + 1U;
		} else if (byte < 0x20U || byte > 0x7eU || byte == '\\') {
			return EINVAL;
		}

		length++;
		if (length >= sizeof(reference->relative))
			return ENAMETOOLONG;
	}

	/* Records the slot and the path. */
	reference->slot = (unsigned)(text[4] - '0');
	memcpy(reference->relative, path, length + 1U);

	/* Reports the parsed reference. */
	return 0;
}

/*
 * Decides the root mode from the root and overlay parameters.
 *
 * A root partition alone means a native root; the two overlay
 * parameters together mean an overlay root.  Any other combination is
 * invalid.
 */
int
kern_boot_source_root_mode(
	const char *rootpart,
	const char *overlay_root,
	const char *overlay_data,
	enum kern_boot_root_mode *mode)
{
	/* Rejects a missing result. */
	if (mode == NULL)
		return EINVAL;
	*mode = KERN_BOOT_ROOT_INVALID;

	/* A root partition excludes the overlay parameters. */
	if (rootpart != NULL) {
		if (overlay_root != NULL || overlay_data != NULL)
			return EINVAL;
		*mode = KERN_BOOT_ROOT_NATIVE;
		return 0;
	}

	/* An overlay root needs both overlay parameters. */
	if (overlay_root == NULL || overlay_data == NULL)
		return EINVAL;
	*mode = KERN_BOOT_ROOT_OVERLAY;

	/* Reports the overlay mode. */
	return 0;
}

/*
 * Tests whether a FAT variant may hold a boot source.
 */
int
kern_boot_source_fat_type_supported(
	enum bootfat_type type)
{
	/* Only FAT16 and FAT32 are supported. */
	if (type == ZEDBSD_FAT16)
		return 1;
	if (type == ZEDBSD_FAT32)
		return 1;

	/* Reports an unsupported variant. */
	return 0;
}

/*
 * Names a boot source failure stage for diagnostics.
 */
const char *
kern_boot_source_failure_stage_name(
	enum kern_boot_source_failure_stage stage)
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

/*
 * Initializes an empty boot source context.
 */
void
kern_boot_source_context_init(
	struct kern_boot_source_context *context)
{
	/* Ignores a missing context. */
	if (context != NULL)
		memset(context, 0, sizeof(*context));
}

/*
 * Unmounts every boot source of a context that was not published.
 *
 * The first unmount error is reported after every slot was tried.
 */
int
kern_boot_source_context_destroy(
	struct kern_boot_source_context *context)
{
	struct kern_boot_source_slot *source;
	unsigned slot;
	int first_error;
	int error;

	first_error = 0;

	/* Rejects a missing context. */
	if (context == NULL)
		return EINVAL;

	/* A published context is an immutable system-lifetime resolver. */
	if (context->runtime_published)
		return EBUSY;

	/* Unmounts the slots in reverse order, keeping the first failure. */
	for (slot = KERN_BOOT_SOURCE_SLOT_COUNT; slot != 0U; slot--) {
		source = &context->slot[slot - 1U];
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

	/* Reports the first unmount failure. */
	return first_error;
}

/*
 * Mounts every boot partition named in the parameters.
 *
 * Slot 0 falls back to the loader's origin partition when no selector
 * names it.  Each partition must be a FAT16 or FAT32 partition that no
 * earlier slot uses.  On failure the context records the failing slot
 * and stage and is destroyed.
 */
int
kern_boot_source_context_mount(
	struct kern_boot_source_context *context,
	const struct kern_boot_parameters *parameters,
	struct disk *loader_origin,
	const char *loader_origin_selector)
{
	struct disk *disk;
	const char *selector;
	enum bootfat_type fat_type;
	unsigned slot;
	unsigned previous;
	int error;

	/* Rejects a missing operand or a context that is published or in use. */
	if (context == NULL || parameters == NULL)
		return EINVAL;
	if (context->runtime_published)
		return EBUSY;
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		if (context->slot[slot].mount != NULL)
			return EBUSY;
	}

	context->failure_stage = KERN_BOOT_SOURCE_FAILURE_NONE;
	context->cleanup_error = 0;

	/* Mounts each named slot, and slot 0 from the loader origin if unnamed. */
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		selector = kern_boot_parameters_boot(parameters, slot);
		disk = NULL;
		if (selector == NULL && slot != 0U)
			continue;

		/* Resolves the selector, the origin selector, or the origin disk. */
		if (selector != NULL) {
			error = kern_boot_source_selector_validate(selector);
			if (error != 0) {
				error = context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_SELECTOR, error);
				return error;
			}

			error = block_identity_resolve(selector, &disk);
			if (error != 0) {
				error = context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_RESOLVE, error);
				return error;
			}
		} else if (loader_origin_selector != NULL) {
			error = kern_boot_source_selector_validate(
			    loader_origin_selector);
			if (error != 0) {
				error = context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_SELECTOR, error);
				return error;
			}

			error = block_identity_resolve(loader_origin_selector, &disk);
			if (error != 0) {
				error = context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_RESOLVE, error);
				return error;
			}
		} else if (loader_origin != NULL) {
			disk = loader_origin;
			disk_ref(disk);
		} else {
			error = context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_RESOLVE, ENXIO);
			return error;
		}

		/* The disk must be a partition no earlier slot uses. */
		if ((disk->d_flags & DISK_PARTITION) == 0) {
			disk_release(disk);
			error = context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_PARTITION, EINVAL);
			return error;
		}

		for (previous = 0; previous < slot; previous++) {
			if (context->slot[previous].configured &&
			    context->slot[previous].disk->d_dev == disk->d_dev)
				break;
		}

		if (previous != slot) {
			disk_release(disk);
			error = context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_DUPLICATE, EEXIST);
			return error;
		}

		/* The partition must carry a supported FAT variant. */
		error = drv_fat_probe_type(disk, &fat_type);
		if (error == 0 && !kern_boot_source_fat_type_supported(fat_type))
			error = EOPNOTSUPP;
		if (error != 0) {
			disk_release(disk);
			error = context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_FILESYSTEM, error);
			return error;
		}

		/* Mounts it privately; the mount holds its own disk reference. */
		error = mount_private("fat", disk, 0, NULL, &context->slot[slot].mount);
		if (error != 0) {
			disk_release(disk);
			error = context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_MOUNT, error);
			return error;
		}

		context->slot[slot].disk = context->slot[slot].mount->m_disk;
		context->slot[slot].runtime_mount = context->slot[slot].mount;
		context->slot[slot].configured = 1U;
		disk_release(disk);
	}

	/* Reports the mounted sources. */
	return 0;
}

/*
 * Resolves a boot reference within the private mounts of a context.
 */
int
kern_boot_source_lookup(
	struct kern_boot_source_context *context,
	const char *text,
	unsigned *slot_out,
	struct path *path_out)
{
	struct kern_boot_source_reference reference;
	int error;

	/* Rejects a missing context or result. */
	if (context == NULL || path_out == NULL)
		return EINVAL;
	path_init(path_out);

	/* Parses the reference; its slot must be mounted. */
	error = kern_boot_source_reference_parse(text, &reference);
	if (error != 0)
		return error;
	if (!context->slot[reference.slot].configured ||
	    context->slot[reference.slot].mount == NULL)
		return ENOENT;

	/* Looks the path up in the slot's mount. */
	error = mount_private_lookup(context->slot[reference.slot].mount,
				     reference.relative, path_out);
	if (error == 0 && slot_out != NULL)
		*slot_out = reference.slot;

	/* Reports why the lookup failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Marks a mounted slot as retained for the running system.
 */
int
kern_boot_source_retain_slot(
	struct kern_boot_source_context *context,
	unsigned slot)
{
	/* Rejects a missing context or a slot out of range. */
	if (context == NULL || slot >= KERN_BOOT_SOURCE_SLOT_COUNT)
		return EINVAL;

	/* Only a mounted slot can be retained. */
	if (!context->slot[slot].configured ||
	    context->slot[slot].mount == NULL)
		return ENOENT;
	context->slot[slot].retained = 1U;

	/* Reports the retained slot. */
	return 0;
}

/*
 * Marks every configured slot as retained.
 */
int
kern_boot_source_retain_configured(
	struct kern_boot_source_context *context)
{
	struct kern_boot_source_slot *source;
	unsigned slot;

	/* Rejects a missing or published context. */
	if (context == NULL || context->runtime_published)
		return EINVAL;

	/* Every configured slot must be completely mounted. */
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		source = &context->slot[slot];
		if (!source->configured)
			continue;
		if (source->mount == NULL ||
		    source->runtime_mount == NULL ||
		    source->disk == NULL)
			return EINVAL;
	}

	/* Retains them all. */
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		if (context->slot[slot].configured)
			context->slot[slot].retained = 1U;
	}

	/* Reports the retained slots. */
	return 0;
}

/*
 * Publishes the retained slots as the system's boot source resolver.
 */
int
kern_boot_source_publish_runtime(
	struct kern_boot_source_context *context)
{
	const struct kern_boot_source_slot *source;
	unsigned slot;

	/* Rejects a missing or already published context. */
	if (context == NULL || context->runtime_published)
		return EINVAL;

	/* Every configured slot must be retained with a runtime mount. */
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		source = &context->slot[slot];
		if (!source->configured)
			continue;
		if (!source->retained ||
		    source->runtime_mount == NULL ||
		    source->disk == NULL)
			return EINVAL;
	}

	/* kern_vfs_init publishes before starting the first userspace process. */
	context->runtime_published = 1U;

	/* Reports the published context. */
	return 0;
}

/*
 * Resolves a boot reference through the published resolver.
 */
int
kern_boot_source_runtime_lookup(
	struct kern_boot_source_context *context,
	const char *text,
	struct path *path_out)
{
	struct kern_boot_source_reference reference;
	struct kern_boot_source_slot *source;
	int error;

	/* Rejects a missing context or result, or an unpublished context. */
	if (context == NULL || path_out == NULL)
		return EINVAL;
	path_init(path_out);
	if (!context->runtime_published)
		return ENXIO;

	/* Parses the reference; its slot must be retained. */
	error = kern_boot_source_reference_parse(text, &reference);
	if (error != 0)
		return error;
	source = &context->slot[reference.slot];
	if (!source->configured ||
	    !source->retained ||
	    source->runtime_mount == NULL)
		return ENOENT;

	/* Looks the path up in the slot's runtime mount. */

	/* Reports why the lookup failed. */
	error = runtime_mount_lookup(source, reference.relative, path_out);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Finds the slot that was mounted from a disk.
 */
int
kern_boot_source_find_disk(
	const struct kern_boot_source_context *context,
	const struct disk *disk,
	unsigned *slot_out)
{
	unsigned slot;

	/* Rejects a missing operand. */
	if (context == NULL || disk == NULL || slot_out == NULL)
		return EINVAL;

	/* Compares the device numbers of the configured slots. */
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		if (context->slot[slot].configured &&
		    context->slot[slot].disk != NULL &&
		    context->slot[slot].disk->d_dev == disk->d_dev) {
			*slot_out = slot;
			return 0;
		}
	}

	/* Reports a disk no slot uses. */
	return ENOENT;
}

/*
 * Turns a slot's private mount into the system root mount.
 */
int
kern_boot_source_promote_root(
	struct kern_boot_source_context *context,
	unsigned slot,
	struct mount **root_out)
{
	struct kern_boot_source_slot *source;
	int error;

	/* Rejects a missing context or a slot out of range. */
	if (context == NULL || slot >= KERN_BOOT_SOURCE_SLOT_COUNT)
		return EINVAL;

	/* Only a mounted slot can be promoted. */
	source = &context->slot[slot];
	if (!source->configured || source->mount == NULL)
		return ENOENT;

	/* Promotes the mount; the slot keeps it only as its runtime mount. */
	error = mount_private_promote_root(source->mount, root_out);
	if (error != 0)
		return error;
	source->mount = NULL;
	source->retained = 1U;
	source->promoted = 1U;

	/* Reports the promoted root. */
	return 0;
}

/*
 * Unmounts every slot that was not retained.
 *
 * The first unmount error is reported after every slot was tried.
 */
int
kern_boot_source_release_unused(
	struct kern_boot_source_context *context)
{
	struct kern_boot_source_slot *source;
	unsigned slot;
	int first_error;
	int error;

	first_error = 0;

	/* Rejects a missing context. */
	if (context == NULL)
		return EINVAL;

	/* Unmounts the unretained slots in reverse order. */
	for (slot = KERN_BOOT_SOURCE_SLOT_COUNT; slot != 0U; slot--) {
		source = &context->slot[slot - 1U];
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

	/* Reports the first unmount failure. */
	return first_error;
}

/* Empties a parameter record. */
static void
parameters_reset(
	struct kern_boot_parameters *parameters)
{
	unsigned index;

	/* Marks every parameter absent and forgets the unknown names. */
	parameters->storage[0] = '\0';
	for (index = 0; index < KERN_BOOT_PARAMETER_COUNT; index++)
		parameters->value_offset[index] =
		    KERN_BOOT_PARAMETER_OFFSET_ABSENT;
	parameters->unknown_count = 0;
	parameters->unknown_name_truncated = 0;
	parameters->unknown_name[0] = '\0';
}

/* Empties a parameter record and passes an error through. */
static int
parse_error(
	struct kern_boot_parameters *parameters,
	int error)
{
	parameters_reset(parameters);

	/* Reports the caller's error. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Compares an unterminated name with a parameter name. */
static int
name_matches(
	const char *name,
	size_t length,
	const struct parameter_name *candidate)
{
	size_t index;

	/* The lengths and every character must agree. */
	if (length != candidate->length)
		return 0;
	for (index = 0; index < length; index++) {
		if (name[index] != candidate->text[index])
			return 0;
	}

	/* Reports a matching name. */
	return 1;
}

/* Finds the key of a parameter name. */
static int
parameter_key(
	const char *name,
	size_t length,
	enum kern_boot_parameter_key *key)
{
	unsigned index;

	/* Searches the known names. */
	for (index = 0; index < KERN_BOOT_PARAMETER_COUNT; index++) {
		if (name_matches(name, length, &parameter_names[index])) {
			*key = (enum kern_boot_parameter_key)index;
			return 1;
		}
	}

	/* Reports an unknown name. */
	return 0;
}

/* Counts an unknown parameter and remembers the first name, truncated. */
static void
record_unknown(
	struct kern_boot_parameters *parameters,
	const char *name,
	size_t length)
{
	size_t copy_length;
	size_t index;

	/* Only the first unknown name is kept. */
	parameters->unknown_count++;
	if (parameters->unknown_count != 1U)
		return;

	/* Copies the name up to the limit, noting a truncation. */
	copy_length = length;
	if (copy_length > KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX) {
		copy_length = KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX;
		parameters->unknown_name_truncated = 1;
	}

	for (index = 0; index < copy_length; index++)
		parameters->unknown_name[index] = name[index];
	parameters->unknown_name[copy_length] = '\0';
}

/* Validates selector text: printable, no slash, bounded, no '=' in a name. */
static int
selector_text(
	const char *text,
	size_t maximum,
	int device_name)
{
	size_t length;
	unsigned char byte;

	/* Rejects missing or empty text. */
	length = 0;
	if (text == NULL || text[0] == '\0')
		return EINVAL;

	/* Checks every character and the length. */
	while (text[length] != '\0') {
		byte = (unsigned char)text[length];
		if (byte < 0x21U ||
		    byte > 0x7eU ||
		    byte == '/' ||
		    (device_name && byte == '='))
			return EINVAL;
		length++;
		if (length >= maximum)
			return ENAMETOOLONG;
	}

	/* Reports valid text. */
	return 0;
}

/* Records a mount failure in the context, destroys it, and passes the error. */
static int
context_fail(
	struct kern_boot_source_context *context,
	unsigned slot,
	enum kern_boot_source_failure_stage stage,
	int error)
{
	/* Remembers where the failure happened for the boot diagnostics. */
	context->failure_slot = slot;
	context->failure_stage = stage;
	context->cleanup_error = kern_boot_source_context_destroy(context);

	/* Reports the caller's error. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Looks a relative path up in a slot's runtime mount. */
static int
runtime_mount_lookup(
	struct kern_boot_source_slot *source,
	const char *relative,
	struct path *result)
{
	struct cwdinfo context;
	struct path root;
	int error;

	/* A private mount is looked up directly. */
	if (!source->promoted) {
		error = mount_private_lookup(source->runtime_mount, relative, result);
		return error;
	}

	/* A promoted root is looked up through a namespace rooted at it. */
	path_init(&root);
	path_set(&root, source->runtime_mount, source->runtime_mount->m_root);
	error = cwdinfo_init(&context, &root);
	path_release(&root);
	if (error != 0)
		return error;
	error = namei_path_at(&context, relative, result);
	cwdinfo_destroy(&context);

	/* Reports why the lookup failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Formats only supported, nonempty GPT signatures into stable selectors. */
static int
provenance_selector(const struct boot_partition_identity *identity, char *output)
{
	static const uint8_t order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
	static const char digits[] = "0123456789abcdef";
	unsigned i, position, nonzero;
	uint8_t byte;

	memset(output, 0, ZEDBSD_BOOT_SOURCE_SELECTOR_SIZE);
	if (identity->index == 0 || identity->block_count == 0 ||
	    identity->first_lba > UINT64_MAX - identity->block_count)
		return EINVAL;
	if (identity->scheme == ZEDBSD_PARTITION_SCHEME_MBR)
		return 0;
	if (identity->scheme != ZEDBSD_PARTITION_SCHEME_GPT)
		return EINVAL;
	nonzero = 0;
	for (i = 0; i < 16; i++)
		nonzero |= identity->signature[i];
	if (nonzero == 0)
		return EINVAL;
	memcpy(output, "PARTUUID=", 9);
	position = 9;
	for (i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			output[position++] = '-';
		byte = identity->signature[order[i]];
		output[position++] = digits[byte >> 4];
		output[position++] = digits[byte & 15U];
	}
	return 0;
}
