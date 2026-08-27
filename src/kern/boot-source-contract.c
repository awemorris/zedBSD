/* Pure boot-source grammar helpers.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/boot-source.h>

#include <errno.h>
#include <string.h>

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
