/* Bounded UEFI LoadOptions to ASCII parameter record conversion. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "load-options.h"

#define EFI_LOAD_OPTION_HEADER_SIZE 6U
#define EFI_LOAD_OPTION_ACTIVE 0x00000001U
#define EFI_LOAD_OPTION_HIDDEN 0x00000008U
#define EFI_LOAD_OPTION_CATEGORY_MASK 0x00001f00U
#define EFI_LOAD_OPTION_CATEGORY_APP 0x00000100U
#define EFI_LOAD_OPTION_ATTRIBUTE_MASK                                      \
	(EFI_LOAD_OPTION_ACTIVE | EFI_LOAD_OPTION_HIDDEN |                   \
	 EFI_LOAD_OPTION_CATEGORY_MASK)
#define EFI_LOAD_OPTION_DESCRIPTOR_MAX 65536U

#define EFI_DEVICE_PATH_HEADER_SIZE 4U
#define EFI_DEVICE_PATH_TYPE_END 0x7fU
#define EFI_DEVICE_PATH_TYPE_END_LEGACY 0xffU
#define EFI_DEVICE_PATH_SUBTYPE_END_INSTANCE 0x01U
#define EFI_DEVICE_PATH_SUBTYPE_END_ENTIRE 0xffU

struct byte_view {
	const uint8_t *data;
	size_t size;
};

static const char *const parameter_names[] = {
	"boot0",       "boot1",       "boot2", "boot3",
	"rootpart",    "overlay-root", "overlay-data",
	"swap0",       "swap1",       "swap2", "swap3",
	"init",
};

static uint16_t
read_le16(const uint8_t *source)
{
	return (uint16_t)((uint16_t)source[0] |
	    ((uint16_t)source[1] << 8));
}

static uint32_t
read_le32(const uint8_t *source)
{
	return (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
	    ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
}

static void
record_header(struct zedbsd_boot_parameter_record *record, size_t length)
{
	for (size_t index = 0; index < sizeof(record->text); index++)
		record->text[index] = '\0';
	record->magic = ZEDBSD_BOOT_PARAMETER_RECORD_MAGIC;
	record->version = ZEDBSD_BOOT_PARAMETER_RECORD_VERSION;
	record->size = sizeof(*record);
	record->flags = ZEDBSD_BOOT_PARAMETER_RECORD_FLAG_TEXT;
	record->length = (uint16_t)length;
	record->reserved = 0;
}

static enum zbl_uefi_load_options_result
record_ascii(struct zedbsd_boot_parameter_record *record, const char *source,
	     size_t capacity)
{
	size_t length;

	if (record == NULL || source == NULL || capacity == 0U)
		return ZBL_UEFI_LOAD_OPTIONS_INVALID_ARGUMENT;
	for (length = 0; length < capacity &&
	     length < ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE; length++) {
		unsigned char byte = (unsigned char)source[length];

		if (byte == 0U)
			break;
		if (byte < 0x20U || byte > 0x7eU)
			return ZBL_UEFI_LOAD_OPTIONS_NON_ASCII;
	}
	if (length == capacity)
		return ZBL_UEFI_LOAD_OPTIONS_MISSING_NUL;
	if (length == ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE)
		return ZBL_UEFI_LOAD_OPTIONS_TOO_LONG;
	record_header(record, length);
	for (size_t index = 0; index <= length; index++)
		record->text[index] = source[index];
	return ZBL_UEFI_LOAD_OPTIONS_OK;
}

static int
recognized_parameter_text(const uint8_t *source, size_t length)
{
	size_t name_start = 0U;
	size_t name_end;

	while (name_start < length &&
	       read_le16(source + name_start * 2U) == ' ')
		name_start++;
	name_end = name_start;
	while (name_end < length &&
	       read_le16(source + name_end * 2U) != '=' &&
	       read_le16(source + name_end * 2U) != ' ')
		name_end++;
	if (name_end == name_start || name_end == length ||
	    read_le16(source + name_end * 2U) != '=')
		return 0;
	for (size_t index = 0;
	     index < sizeof(parameter_names) / sizeof(parameter_names[0]);
	     index++) {
		const char *candidate = parameter_names[index];
		size_t candidate_length = 0U;
		int matches = 1;

		while (candidate[candidate_length] != '\0')
			candidate_length++;
		if (candidate_length != name_end - name_start)
			continue;
		for (size_t offset = 0; offset < candidate_length; offset++) {
			if (read_le16(source + (name_start + offset) * 2U) !=
			    (uint8_t)candidate[offset]) {
				matches = 0;
				break;
			}
		}
		if (matches)
			return 1;
	}
	return 0;
}

static enum zbl_uefi_load_options_result
record_utf16(struct zedbsd_boot_parameter_record *record,
	     const uint8_t *source, size_t source_size)
{
	size_t units;
	size_t length;
	int terminated;

	if (record == NULL || (source == NULL && source_size != 0U))
		return ZBL_UEFI_LOAD_OPTIONS_INVALID_ARGUMENT;
	if (source_size == 0U)
		return ZBL_UEFI_LOAD_OPTIONS_EMPTY;
	if ((source_size & 1U) != 0U)
		return ZBL_UEFI_LOAD_OPTIONS_ODD_SIZE;
	units = source_size / 2U;
	terminated = read_le16(source + (units - 1U) * 2U) == 0U;
	length = units - (terminated ? 1U : 0U);
	if (length == 0U)
		return ZBL_UEFI_LOAD_OPTIONS_EMPTY;
	if (length > ZEDBSD_BOOT_PARAMETERS_TEXT_MAX)
		return ZBL_UEFI_LOAD_OPTIONS_TOO_LONG;
	for (size_t index = 0; index < length; index++) {
		uint16_t code = read_le16(source + index * 2U);

		if (code == 0U)
			return ZBL_UEFI_LOAD_OPTIONS_EMBEDDED_NUL;
		if (code < 0x20U || code > 0x7eU)
			return ZBL_UEFI_LOAD_OPTIONS_NON_ASCII;
	}
	if (!recognized_parameter_text(source, length))
		return ZBL_UEFI_LOAD_OPTIONS_UNRECOGNIZED;
	record_header(record, length);
	for (size_t index = 0; index < length; index++)
		record->text[index] =
		    (char)read_le16(source + index * 2U);
	record->text[length] = '\0';
	return ZBL_UEFI_LOAD_OPTIONS_OK;
}

/*
 * Some x86 firmware passes the complete packed EFI_LOAD_OPTION descriptor
 * instead of its OptionalData.  There is no descriptor magic, so accept this
 * compatibility form only after validating every variable-length boundary.
 */
static int
unpack_efi_load_option(const uint8_t *source, size_t source_size,
		       struct byte_view *optional_data)
{
	uint32_t attributes;
	uint32_t category;
	size_t description_end;
	size_t path_length;
	size_t path_start;
	size_t path_end;
	size_t position;
	int path_has_node = 0;
	int ended_entire = 0;

	if (source == NULL || optional_data == NULL ||
	    source_size < EFI_LOAD_OPTION_HEADER_SIZE + 2U +
		EFI_DEVICE_PATH_HEADER_SIZE ||
	    source_size > EFI_LOAD_OPTION_DESCRIPTOR_MAX)
		return 0;
	attributes = read_le32(source);
	if ((attributes & ~EFI_LOAD_OPTION_ATTRIBUTE_MASK) != 0U)
		return 0;
	category = attributes & EFI_LOAD_OPTION_CATEGORY_MASK;
	if (category != 0U && category != EFI_LOAD_OPTION_CATEGORY_APP)
		return 0;

	description_end = EFI_LOAD_OPTION_HEADER_SIZE;
	for (;;) {
		if (description_end + 2U > source_size)
			return 0;
		if (read_le16(source + description_end) == 0U) {
			description_end += 2U;
			break;
		}
		description_end += 2U;
	}

	path_length = read_le16(source + 4U);
	if (path_length < EFI_DEVICE_PATH_HEADER_SIZE ||
	    path_length > source_size - description_end)
		return 0;
	path_start = description_end;
	path_end = path_start + path_length;
	position = path_start;
	while (position < path_end) {
		uint8_t type;
		uint8_t subtype;
		size_t node_length;

		if (path_end - position < EFI_DEVICE_PATH_HEADER_SIZE)
			return 0;
		type = source[position];
		subtype = source[position + 1U];
		node_length = read_le16(source + position + 2U);
		if (node_length < EFI_DEVICE_PATH_HEADER_SIZE ||
		    node_length > path_end - position)
			return 0;
		position += node_length;
		if (type != EFI_DEVICE_PATH_TYPE_END &&
		    type != EFI_DEVICE_PATH_TYPE_END_LEGACY) {
			path_has_node = 1;
			ended_entire = 0;
			continue;
		}
		if (node_length != EFI_DEVICE_PATH_HEADER_SIZE ||
		    !path_has_node ||
		    (subtype != EFI_DEVICE_PATH_SUBTYPE_END_INSTANCE &&
		     subtype != EFI_DEVICE_PATH_SUBTYPE_END_ENTIRE))
			return 0;
		path_has_node = 0;
		ended_entire =
		    subtype == EFI_DEVICE_PATH_SUBTYPE_END_ENTIRE;
		if (ended_entire && position < path_end) {
			/* The packed list may contain another complete path. */
			ended_entire = 0;
		}
	}
	if (!ended_entire || position != path_end)
		return 0;
	optional_data->data = source + path_end;
	optional_data->size = source_size - path_end;
	return 1;
}

enum zbl_uefi_load_options_result
zbl_uefi_load_options_record(
	struct zedbsd_boot_parameter_record *record,
	const void *load_options, uint32_t load_options_size,
	const char *image_parameters, size_t image_parameter_capacity)
{
	const uint8_t *source = load_options;
	struct byte_view optional_data;
	enum zbl_uefi_load_options_result result;

	if (record == NULL || image_parameters == NULL ||
	    image_parameter_capacity == 0U ||
	    (load_options == NULL && load_options_size != 0U))
		return ZBL_UEFI_LOAD_OPTIONS_INVALID_ARGUMENT;
	if (load_options_size == 0U)
		return record_ascii(record, image_parameters,
		    image_parameter_capacity);

	/* Standards-compliant LoadOptions is OptionalData, so try it first. */
	result = record_utf16(record, source, load_options_size);
	if (result == ZBL_UEFI_LOAD_OPTIONS_OK)
		return result;
	if (unpack_efi_load_option(source, load_options_size, &optional_data)) {
		if (optional_data.size == 0U) {
			result = record_ascii(record, image_parameters,
			    image_parameter_capacity);
		} else {
			result = record_utf16(record, optional_data.data,
			    optional_data.size);
			if (result == ZBL_UEFI_LOAD_OPTIONS_EMPTY)
				result = record_ascii(record, image_parameters,
				    image_parameter_capacity);
		}
		return result == ZBL_UEFI_LOAD_OPTIONS_OK ?
		    ZBL_UEFI_LOAD_OPTIONS_DESCRIPTOR : result;
	}
	if (result == ZBL_UEFI_LOAD_OPTIONS_EMPTY)
		return record_ascii(record, image_parameters,
		    image_parameter_capacity);
	return result;
}

const char *
zbl_uefi_load_options_result_name(enum zbl_uefi_load_options_result result)
{
	switch (result) {
	case ZBL_UEFI_LOAD_OPTIONS_OK:
		return "ok";
	case ZBL_UEFI_LOAD_OPTIONS_INVALID_ARGUMENT:
		return "invalid-argument";
	case ZBL_UEFI_LOAD_OPTIONS_EMPTY:
		return "empty";
	case ZBL_UEFI_LOAD_OPTIONS_ODD_SIZE:
		return "odd-size";
	case ZBL_UEFI_LOAD_OPTIONS_TOO_LONG:
		return "too-long";
	case ZBL_UEFI_LOAD_OPTIONS_MISSING_NUL:
		return "missing-nul";
	case ZBL_UEFI_LOAD_OPTIONS_EMBEDDED_NUL:
		return "embedded-nul";
	case ZBL_UEFI_LOAD_OPTIONS_NON_ASCII:
		return "non-ascii";
	case ZBL_UEFI_LOAD_OPTIONS_UNRECOGNIZED:
		return "unrecognized";
	case ZBL_UEFI_LOAD_OPTIONS_DESCRIPTOR:
		return "descriptor-optional-data";
	}
	return "unknown";
}
