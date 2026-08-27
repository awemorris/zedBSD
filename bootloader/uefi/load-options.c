/* Strict UEFI LoadOptions to bounded ASCII parameter record conversion. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "load-options.h"

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

	if (source == NULL || capacity == 0U)
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

enum zbl_uefi_load_options_result
zbl_uefi_load_options_record(
	struct zedbsd_boot_parameter_record *record,
	const void *load_options, uint32_t load_options_size,
	const char *image_parameters, size_t image_parameter_capacity)
{
	const uint16_t *source = load_options;
	size_t units, length, index;

	if (record == NULL || image_parameters == NULL ||
	    image_parameter_capacity == 0U ||
	    (load_options == NULL && load_options_size != 0U))
		return ZBL_UEFI_LOAD_OPTIONS_INVALID_ARGUMENT;
	if (load_options_size == 0U)
		return record_ascii(record, image_parameters,
		    image_parameter_capacity);
	if ((load_options_size & 1U) != 0U)
		return ZBL_UEFI_LOAD_OPTIONS_ODD_SIZE;
	units = load_options_size / sizeof(*source);
	if (units == 0U || source[units - 1U] != 0U)
		return ZBL_UEFI_LOAD_OPTIONS_MISSING_NUL;
	length = units - 1U;
	if (length > ZEDBSD_BOOT_PARAMETERS_TEXT_MAX)
		return ZBL_UEFI_LOAD_OPTIONS_TOO_LONG;
	if (length == 0U)
		return record_ascii(record, image_parameters,
		    image_parameter_capacity);
	for (index = 0; index < length; index++) {
		uint16_t code = source[index];

		if (code == 0U)
			return ZBL_UEFI_LOAD_OPTIONS_EMBEDDED_NUL;
		if (code < 0x20U || code > 0x7eU)
			return ZBL_UEFI_LOAD_OPTIONS_NON_ASCII;
	}
	record_header(record, length);
	for (index = 0; index < length; index++)
		record->text[index] = (char)source[index];
	record->text[length] = '\0';
	return ZBL_UEFI_LOAD_OPTIONS_OK;
}

const char *
zbl_uefi_load_options_result_name(enum zbl_uefi_load_options_result result)
{
	switch (result) {
	case ZBL_UEFI_LOAD_OPTIONS_OK:
		return "ok";
	case ZBL_UEFI_LOAD_OPTIONS_INVALID_ARGUMENT:
		return "invalid-argument";
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
	}
	return "unknown";
}
